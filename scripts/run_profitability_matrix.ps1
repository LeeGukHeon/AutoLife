param(
    [string]$ExePath = ".\build\Release\AutoLifeTrading.exe",
    [string]$ConfigPath = ".\build\Release\config\config.json",
    [string]$SourceConfigPath = ".\config\config.json",
    [string]$DataDir = ".\data\backtest",
    [string[]]$DatasetNames = @("simulation_2000.csv", "simulation_large.csv"),
    [string]$OutputCsv = ".\build\Release\logs\profitability_matrix.csv",
    [string]$OutputProfileCsv = ".\build\Release\logs\profitability_profile_summary.csv",
    [string]$OutputJson = ".\build\Release\logs\profitability_gate_report.json",
    [switch]$IncludeWalkForward,
    [string]$WalkForwardScript = ".\scripts\walk_forward_validate.ps1",
    [string]$WalkForwardInput = ".\data\backtest\simulation_2000.csv",
    [string]$WalkForwardOutputJson = ".\build\Release\logs\walk_forward_profitability_matrix.json",
    [double]$MinProfitFactor = 1.00,
    [double]$MinExpectancyKrw = 0.0,
    [double]$MaxDrawdownPct = 12.0,
    [double]$MinProfitableRatio = 0.55,
    [int]$MinAvgTrades = 10,
    [switch]$ExcludeLowTradeRunsForGate,
    [int]$MinTradesPerRunForGate = 5,
    [double]$CoreVsLegacyMinProfitFactorDelta = -0.05,
    [double]$CoreVsLegacyMinExpectancyDeltaKrw = -5.0,
    [double]$CoreVsLegacyMinTotalProfitDeltaKrw = -10000.0,
    [switch]$FailOnGate
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

function Ensure-ConfigScaffold {
    param($ConfigObject)

    if ($null -eq $ConfigObject.trading) {
        $ConfigObject | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
    }

    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_plane_bridge" -Value $false
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_policy_plane" -Value $false
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_risk_plane" -Value $false
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_execution_plane" -Value $false
}

function Apply-ProfileFlags {
    param(
        $ConfigObject,
        [bool]$Bridge,
        [bool]$Policy,
        [bool]$Risk,
        [bool]$Execution
    )

    Ensure-ConfigScaffold -ConfigObject $ConfigObject
    $ConfigObject.trading.enable_core_plane_bridge = $Bridge
    $ConfigObject.trading.enable_core_policy_plane = $Policy
    $ConfigObject.trading.enable_core_risk_plane = $Risk
    $ConfigObject.trading.enable_core_execution_plane = $Execution
}

function Invoke-BacktestJson {
    param(
        [string]$ExeFile,
        [string]$DatasetPath
    )

    $raw = & $ExeFile --backtest $DatasetPath --json 2>&1
    $jsonLine = $null
    for ($i = $raw.Count - 1; $i -ge 0; $i--) {
        $line = [string]$raw[$i]
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith("{") -and $trimmed.EndsWith("}")) {
            $jsonLine = $trimmed
            break
        }
    }
    if ($null -eq $jsonLine) {
        return $null
    }
    try {
        return ($jsonLine | ConvertFrom-Json -ErrorAction Stop)
    } catch {
        return $null
    }
}

$resolvedExePath = Resolve-OrThrow -PathValue $ExePath -Label "Executable"
$resolvedConfigPath = if (Test-Path $ConfigPath) { Resolve-OrThrow -PathValue $ConfigPath -Label "Config" } else { $ConfigPath }
$resolvedSourceConfigPath = Resolve-OrThrow -PathValue $SourceConfigPath -Label "Source config"
$resolvedDataDir = Resolve-OrThrow -PathValue $DataDir -Label "Data directory"

$resolvedOutputCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputCsv))
$resolvedOutputProfileCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputProfileCsv))
$resolvedOutputJson = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputJson))
$resolvedWalkForwardOutputJson = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $WalkForwardOutputJson))
Ensure-ParentDirectory -PathValue $resolvedOutputCsv
Ensure-ParentDirectory -PathValue $resolvedOutputProfileCsv
Ensure-ParentDirectory -PathValue $resolvedOutputJson
Ensure-ParentDirectory -PathValue $resolvedWalkForwardOutputJson

if (-not (Test-Path $resolvedConfigPath)) {
    Ensure-ParentDirectory -PathValue $resolvedConfigPath
    Copy-Item -Path $resolvedSourceConfigPath -Destination $resolvedConfigPath -Force
}

$datasetPaths = New-Object "System.Collections.Generic.List[string]"
foreach ($datasetName in $DatasetNames) {
    if ([string]::IsNullOrWhiteSpace($datasetName)) {
        continue
    }
    $candidate = if ([System.IO.Path]::IsPathRooted($datasetName)) {
        $datasetName
    } else {
        Join-Path $resolvedDataDir $datasetName
    }
    if (-not (Test-Path $candidate)) {
        throw "Dataset not found: $candidate"
    }
    $datasetPaths.Add((Resolve-OrThrow -PathValue $candidate -Label "Dataset"))
}
if ($datasetPaths.Count -eq 0) {
    throw "No datasets configured. Set -DatasetNames."
}

$profileSpecs = @(
    [pscustomobject]@{
        profile_id = "legacy_default"
        description = "All core plane flags disabled."
        bridge = $false
        policy = $false
        risk = $false
        execution = $false
    },
    [pscustomobject]@{
        profile_id = "core_bridge_only"
        description = "Core bridge enabled, policy/risk/execution planes disabled."
        bridge = $true
        policy = $false
        risk = $false
        execution = $false
    },
    [pscustomobject]@{
        profile_id = "core_policy_risk"
        description = "Core bridge + policy + risk enabled, execution plane disabled."
        bridge = $true
        policy = $true
        risk = $true
        execution = $false
    },
    [pscustomobject]@{
        profile_id = "core_full"
        description = "All core plane flags enabled."
        bridge = $true
        policy = $true
        risk = $true
        execution = $true
    }
)

$originalConfigRaw = Get-Content -Path $resolvedConfigPath -Encoding UTF8 | Out-String
$rows = New-Object "System.Collections.Generic.List[object]"

try {
    foreach ($profile in $profileSpecs) {
        $cfg = $originalConfigRaw | ConvertFrom-Json
        Apply-ProfileFlags -ConfigObject $cfg -Bridge ([bool]$profile.bridge) -Policy ([bool]$profile.policy) -Risk ([bool]$profile.risk) -Execution ([bool]$profile.execution)
        ($cfg | ConvertTo-Json -Depth 32) | Set-Content -Path $resolvedConfigPath -Encoding UTF8

        foreach ($datasetPath in $datasetPaths) {
            $result = Invoke-BacktestJson -ExeFile $resolvedExePath -DatasetPath $datasetPath
            if ($null -eq $result) {
                throw "Backtest JSON parsing failed: profile=$($profile.profile_id), dataset=$datasetPath"
            }

            $profit = [double]$result.total_profit
            $profitFactor = [double]$result.profit_factor
            $expectancy = [double]$result.expectancy_krw
            $runMaxDrawdownPct = [Math]::Round(([double]$result.max_drawdown * 100.0), 4)
            $totalTrades = [int]$result.total_trades
            $winRatePct = [Math]::Round(([double]$result.win_rate * 100.0), 4)

            $rows.Add([pscustomobject]@{
                profile_id = [string]$profile.profile_id
                profile_description = [string]$profile.description
                dataset = [System.IO.Path]::GetFileName($datasetPath)
                core_bridge_enabled = [bool]$profile.bridge
                core_policy_enabled = [bool]$profile.policy
                core_risk_enabled = [bool]$profile.risk
                core_execution_enabled = [bool]$profile.execution
                total_profit_krw = [Math]::Round($profit, 4)
                profit_factor = [Math]::Round($profitFactor, 4)
                expectancy_krw = [Math]::Round($expectancy, 4)
                max_drawdown_pct = $runMaxDrawdownPct
                total_trades = $totalTrades
                win_rate_pct = $winRatePct
                profitable = ($profit -gt 0.0)
                gate_trade_eligible = if ($ExcludeLowTradeRunsForGate.IsPresent) { $totalTrades -ge $MinTradesPerRunForGate } else { $true }
            }) | Out-Null
        }
    }
}
finally {
    $originalConfigRaw | Set-Content -Path $resolvedConfigPath -Encoding UTF8
}

if ($rows.Count -eq 0) {
    throw "No profitability rows generated."
}

$sortedRows = @($rows | Sort-Object profile_id, dataset)
$sortedRows | Export-Csv -Path $resolvedOutputCsv -NoTypeInformation -Encoding UTF8

$profileSummaries = @()
foreach ($group in ($sortedRows | Group-Object profile_id)) {
    $items = @($group.Group)
    $gateItems = if ($ExcludeLowTradeRunsForGate.IsPresent) {
        @($items | Where-Object { $_.gate_trade_eligible })
    } else {
        @($items)
    }
    $runCount = @($items).Count
    $gateRunCount = @($gateItems).Count
    $excludedByTradeCountRuns = $runCount - $gateRunCount
    $profitableCount = @($gateItems | Where-Object { $_.profitable }).Count
    $profitableRatio = if ($gateRunCount -gt 0) { [Math]::Round(($profitableCount / [double]$gateRunCount), 4) } else { 0.0 }
    $avgProfitFactorRaw = $null
    $avgExpectancyRaw = $null
    $peakDrawdownRaw = $null
    $avgTradesRaw = $null
    $sumProfitRaw = $null
    if ($gateRunCount -gt 0) {
        $avgProfitFactorRaw = ($gateItems | Measure-Object -Property profit_factor -Average).Average
        $avgExpectancyRaw = ($gateItems | Measure-Object -Property expectancy_krw -Average).Average
        $peakDrawdownRaw = ($gateItems | Measure-Object -Property max_drawdown_pct -Maximum).Maximum
        $avgTradesRaw = ($gateItems | Measure-Object -Property total_trades -Average).Average
        $sumProfitRaw = ($gateItems | Measure-Object -Property total_profit_krw -Sum).Sum
    }
    $avgProfitFactor = if ($null -eq $avgProfitFactorRaw) { 0.0 } else { [Math]::Round([double]$avgProfitFactorRaw, 4) }
    $avgExpectancy = if ($null -eq $avgExpectancyRaw) { 0.0 } else { [Math]::Round([double]$avgExpectancyRaw, 4) }
    $peakDrawdown = if ($null -eq $peakDrawdownRaw) { 0.0 } else { [Math]::Round([double]$peakDrawdownRaw, 4) }
    $avgTrades = if ($null -eq $avgTradesRaw) { 0.0 } else { [Math]::Round([double]$avgTradesRaw, 4) }
    $sumProfit = if ($null -eq $sumProfitRaw) { 0.0 } else { [Math]::Round([double]$sumProfitRaw, 4) }

    $gateSamplePass = ($gateRunCount -gt 0)
    $gateProfitFactorPass = ($avgProfitFactor -ge $MinProfitFactor)
    $gateExpectancyPass = ($avgExpectancy -ge $MinExpectancyKrw)
    $gateDrawdownPass = ($peakDrawdown -le $MaxDrawdownPct)
    $gateProfitableRatioPass = ($profitableRatio -ge $MinProfitableRatio)
    $gateTradesPass = ($avgTrades -ge $MinAvgTrades)
    $gatePass = (
        $gateSamplePass -and
        $gateProfitFactorPass -and
        $gateExpectancyPass -and
        $gateDrawdownPass -and
        $gateProfitableRatioPass -and
        $gateTradesPass
    )

    $profileSummaries += [pscustomobject]@{
        profile_id = $group.Name
        runs = $runCount
        runs_used_for_gate = $gateRunCount
        excluded_low_trade_runs = $excludedByTradeCountRuns
        profitable_runs = $profitableCount
        profitable_ratio = $profitableRatio
        avg_profit_factor = $avgProfitFactor
        avg_expectancy_krw = $avgExpectancy
        peak_max_drawdown_pct = $peakDrawdown
        avg_total_trades = $avgTrades
        total_profit_sum_krw = $sumProfit
        gate_sample_pass = $gateSamplePass
        gate_profit_factor_pass = $gateProfitFactorPass
        gate_expectancy_pass = $gateExpectancyPass
        gate_drawdown_pass = $gateDrawdownPass
        gate_profitable_ratio_pass = $gateProfitableRatioPass
        gate_trades_pass = $gateTradesPass
        gate_pass = $gatePass
    }
}
$profileSummaries = @($profileSummaries | Sort-Object profile_id)
$profileSummaries | Export-Csv -Path $resolvedOutputProfileCsv -NoTypeInformation -Encoding UTF8

$legacySummary = $profileSummaries | Where-Object { $_.profile_id -eq "legacy_default" } | Select-Object -First 1
$coreFullSummary = $profileSummaries | Where-Object { $_.profile_id -eq "core_full" } | Select-Object -First 1

$coreVsLegacy = [ordered]@{
    comparison_available = ($null -ne $legacySummary -and $null -ne $coreFullSummary)
    baseline_profile = "legacy_default"
    candidate_profile = "core_full"
}

if ($coreVsLegacy.comparison_available) {
    $deltaProfitFactor = [Math]::Round(([double]$coreFullSummary.avg_profit_factor - [double]$legacySummary.avg_profit_factor), 4)
    $deltaExpectancy = [Math]::Round(([double]$coreFullSummary.avg_expectancy_krw - [double]$legacySummary.avg_expectancy_krw), 4)
    $deltaTotalProfit = [Math]::Round(([double]$coreFullSummary.total_profit_sum_krw - [double]$legacySummary.total_profit_sum_krw), 4)

    $coreVsLegacy.delta_avg_profit_factor = $deltaProfitFactor
    $coreVsLegacy.delta_avg_expectancy_krw = $deltaExpectancy
    $coreVsLegacy.delta_total_profit_sum_krw = $deltaTotalProfit
    $coreVsLegacy.min_delta_avg_profit_factor = $CoreVsLegacyMinProfitFactorDelta
    $coreVsLegacy.min_delta_avg_expectancy_krw = $CoreVsLegacyMinExpectancyDeltaKrw
    $coreVsLegacy.min_delta_total_profit_sum_krw = $CoreVsLegacyMinTotalProfitDeltaKrw
    $coreVsLegacy.gate_profit_factor_delta_pass = ($deltaProfitFactor -ge $CoreVsLegacyMinProfitFactorDelta)
    $coreVsLegacy.gate_expectancy_delta_pass = ($deltaExpectancy -ge $CoreVsLegacyMinExpectancyDeltaKrw)
    $coreVsLegacy.gate_total_profit_delta_pass = ($deltaTotalProfit -ge $CoreVsLegacyMinTotalProfitDeltaKrw)
    $coreVsLegacy.gate_pass = (
        $coreVsLegacy.gate_profit_factor_delta_pass -and
        $coreVsLegacy.gate_expectancy_delta_pass -and
        $coreVsLegacy.gate_total_profit_delta_pass
    )
} else {
    $coreVsLegacy.gate_pass = $false
}

$walkForward = $null
if ($IncludeWalkForward.IsPresent) {
    $resolvedWalkForwardScript = Resolve-OrThrow -PathValue $WalkForwardScript -Label "Walk-forward script"
    $resolvedWalkForwardInput = Resolve-OrThrow -PathValue $WalkForwardInput -Label "Walk-forward input"

    $cfg = $originalConfigRaw | ConvertFrom-Json
    Apply-ProfileFlags -ConfigObject $cfg -Bridge $true -Policy $true -Risk $true -Execution $true
    ($cfg | ConvertTo-Json -Depth 32) | Set-Content -Path $resolvedConfigPath -Encoding UTF8
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $resolvedWalkForwardScript `
            -ExePath $resolvedExePath `
            -InputCsv $resolvedWalkForwardInput `
            -OutputJson $resolvedWalkForwardOutputJson | Out-Null
    } finally {
        $originalConfigRaw | Set-Content -Path $resolvedConfigPath -Encoding UTF8
    }

    if (Test-Path $resolvedWalkForwardOutputJson) {
        try {
            $walkForward = Get-Content -Raw $resolvedWalkForwardOutputJson | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $walkForward = $null
        }
    }
}

$profileGatePass = @($profileSummaries | Where-Object { -not $_.gate_pass }).Count -eq 0
$overallGatePass = ($profileGatePass -and [bool]$coreVsLegacy.gate_pass)

$report = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    inputs = [ordered]@{
        exe_path = $resolvedExePath
        config_path = $resolvedConfigPath
        source_config_path = $resolvedSourceConfigPath
        data_dir = $resolvedDataDir
        datasets = $datasetPaths
    }
    thresholds = [ordered]@{
        min_profit_factor = $MinProfitFactor
        min_expectancy_krw = $MinExpectancyKrw
        max_drawdown_pct = $MaxDrawdownPct
        min_profitable_ratio = $MinProfitableRatio
        min_avg_trades = $MinAvgTrades
        exclude_low_trade_runs_for_gate = [bool]$ExcludeLowTradeRunsForGate
        min_trades_per_run_for_gate = $MinTradesPerRunForGate
        core_vs_legacy_min_profit_factor_delta = $CoreVsLegacyMinProfitFactorDelta
        core_vs_legacy_min_expectancy_delta_krw = $CoreVsLegacyMinExpectancyDeltaKrw
        core_vs_legacy_min_total_profit_delta_krw = $CoreVsLegacyMinTotalProfitDeltaKrw
    }
    profile_gate_pass = $profileGatePass
    core_vs_legacy = $coreVsLegacy
    overall_gate_pass = $overallGatePass
    profile_summaries = $profileSummaries
    matrix_rows = $sortedRows
    walk_forward = if ($IncludeWalkForward.IsPresent) { $walkForward } else { $null }
}

($report | ConvertTo-Json -Depth 10) | Set-Content -Path $resolvedOutputJson -Encoding UTF8

Write-Host "[ProfitabilityMatrix] Completed"
Write-Host "matrix_csv=$resolvedOutputCsv"
Write-Host "profile_csv=$resolvedOutputProfileCsv"
Write-Host "gate_report=$resolvedOutputJson"
if ($IncludeWalkForward.IsPresent) {
    Write-Host "walk_forward_report=$resolvedWalkForwardOutputJson"
}
Write-Host "overall_gate_pass=$overallGatePass"

if ($FailOnGate.IsPresent -and -not $overallGatePass) {
    Write-Host "[ProfitabilityMatrix] FAILED (overall gate)"
    exit 1
}

exit 0
