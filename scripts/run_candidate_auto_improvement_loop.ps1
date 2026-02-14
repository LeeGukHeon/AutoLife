param(
    [string]$RealDataLoopScript = ".\scripts\run_realdata_candidate_loop.ps1",
    [string]$TuneScript = ".\scripts\tune_candidate_gate_trade_density.ps1",
    [string]$LossAnalysisScript = ".\scripts\analyze_loss_contributors.ps1",
    [string]$BuildConfigPath = ".\build\Release\config\config.json",
    [string]$SourceConfigPath = ".\config\config.json",
    [string]$GateReportJson = ".\build\Release\logs\profitability_gate_report_realdata.json",
    [string]$TuneSummaryJson = ".\build\Release\logs\candidate_trade_density_tuning_summary.json",
    [string]$IterationCsv = ".\build\Release\logs\candidate_auto_improvement_iterations.csv",
    [string]$SummaryJson = ".\build\Release\logs\candidate_auto_improvement_summary.json",
    [int]$MaxIterations = 4,
    [int]$MaxConsecutiveNoImprovement = 2,
    [int]$MaxRuntimeMinutes = 120,
    [double]$MinProfitFactor = 1.00,
    [double]$MinExpectancyKrw = 0.0,
    [double]$MinProfitableRatio = 0.55,
    [double]$MinAvgTrades = 10.0,
    [double]$ImprovementEpsilon = 0.05,
    [ValidateSet("legacy_only", "diverse_light", "diverse_wide", "quality_focus")]
    [string]$TuneScenarioMode = "diverse_light",
    [int]$TuneMaxScenarios = 16,
    [switch]$TuneIncludeLegacyScenarios,
    [switch]$FetchEachIteration,
    [switch]$SkipTunePhase,
    [switch]$RunLossAnalysis,
    [switch]$SyncSourceConfig
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

function Invoke-PsFile {
    param(
        [string]$ScriptPath,
        [string[]]$Args,
        [string]$Label
    )
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath)
    if ($null -ne $Args -and $Args.Count -gt 0) {
        $argList += $Args
    }
    & powershell @argList
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "$Label failed (exit=$code)"
    }
}

function Invoke-ScriptInProcess {
    param(
        [string]$ScriptPath,
        [hashtable]$ScriptArgs,
        [string]$Label
    )
    if ($null -eq $ScriptArgs) {
        $ScriptArgs = @{}
    }
    $global:LASTEXITCODE = 0
    & $ScriptPath @ScriptArgs
    $code = $LASTEXITCODE
    if ($null -ne $code -and $code -ne 0) {
        throw "$Label failed (exit=$code)"
    }
}

function Compute-ObjectiveScore {
    param(
        [double]$AvgProfitFactor,
        [double]$AvgExpectancyKrw,
        [double]$ProfitableRatio,
        [double]$AvgTotalTrades,
        [double]$MinTradesFloor
    )
    $score = 0.0
    $score += ($AvgExpectancyKrw * 8.0)
    $score += ($ProfitableRatio * 1200.0)
    $score += (($AvgProfitFactor - 1.0) * 220.0)
    $score += ([Math]::Min($AvgTotalTrades, 20.0) * 18.0)

    if ($AvgTotalTrades -lt $MinTradesFloor) {
        $score -= (500.0 + (($MinTradesFloor - $AvgTotalTrades) * 40.0))
    }
    if ($AvgProfitFactor -lt 1.0) {
        $score -= ((1.0 - $AvgProfitFactor) * 300.0)
    }
    if ($AvgExpectancyKrw -lt 0.0) {
        $score += ($AvgExpectancyKrw * 3.0)
    }
    return [Math]::Round($score, 6)
}

function Test-TargetSatisfied {
    param(
        $CoreSummary,
        [double]$MinPf,
        [double]$MinExp,
        [double]$MinRatio,
        [double]$MinTrades
    )
    if ($null -eq $CoreSummary) {
        return $false
    }
    return (
        [double]$CoreSummary.avg_profit_factor -ge $MinPf -and
        [double]$CoreSummary.avg_expectancy_krw -ge $MinExp -and
        [double]$CoreSummary.profitable_ratio -ge $MinRatio -and
        [double]$CoreSummary.avg_total_trades -ge $MinTrades
    )
}

function Get-CoreSnapshot {
    param(
        [string]$ReportPath,
        [double]$MinTradesFloor
    )
    $report = Get-Content -Raw -Path $ReportPath -Encoding UTF8 | ConvertFrom-Json
    $core = $report.profile_summaries | Where-Object { $_.profile_id -eq "core_full" } | Select-Object -First 1
    if ($null -eq $core) {
        throw "core_full summary not found: $ReportPath"
    }

    $objective = Compute-ObjectiveScore `
        -AvgProfitFactor ([double]$core.avg_profit_factor) `
        -AvgExpectancyKrw ([double]$core.avg_expectancy_krw) `
        -ProfitableRatio ([double]$core.profitable_ratio) `
        -AvgTotalTrades ([double]$core.avg_total_trades) `
        -MinTradesFloor $MinTradesFloor

    return [pscustomobject]@{
        report = $report
        core = $core
        objective_score = $objective
        overall_gate_pass = [bool]$report.overall_gate_pass
        core_vs_legacy_gate_pass = if ($null -ne $report.core_vs_legacy) { [bool]$report.core_vs_legacy.gate_pass } else { $false }
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

function Apply-ComboToConfigObject {
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

function Apply-ComboToConfigFiles {
    param(
        [string]$BuildConfig,
        [string]$SourceConfig,
        $Combo,
        [bool]$SyncSource
    )
    $buildRaw = Get-Content -Raw -Path $BuildConfig -Encoding UTF8
    $buildCfg = $buildRaw | ConvertFrom-Json
    Apply-ComboToConfigObject -ConfigObject $buildCfg -Combo $Combo
    ($buildCfg | ConvertTo-Json -Depth 32) | Set-Content -Path $BuildConfig -Encoding UTF8

    if ($SyncSource -and (Test-Path $SourceConfig)) {
        $sourceRaw = Get-Content -Raw -Path $SourceConfig -Encoding UTF8
        $sourceCfg = $sourceRaw | ConvertFrom-Json
        Apply-ComboToConfigObject -ConfigObject $sourceCfg -Combo $Combo
        ($sourceCfg | ConvertTo-Json -Depth 32) | Set-Content -Path $SourceConfig -Encoding UTF8
    }
}

function Select-BestComboFromTuneSummary {
    param(
        [string]$TuneSummaryJsonPath,
        [double]$MinTradesFloor
    )
    $payload = Get-Content -Raw -Path $TuneSummaryJsonPath -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $payload.summary -or $null -eq $payload.combos) {
        throw "Invalid tune summary json: $TuneSummaryJsonPath"
    }

    $comboById = @{}
    foreach ($combo in $payload.combos) {
        $comboById[[string]$combo.combo_id] = $combo
    }

    $candidates = New-Object "System.Collections.Generic.List[object]"
    foreach ($row in $payload.summary) {
        $comboId = [string]$row.combo_id
        if (-not $comboById.ContainsKey($comboId)) {
            continue
        }
        $objective = Compute-ObjectiveScore `
            -AvgProfitFactor ([double]$row.avg_profit_factor) `
            -AvgExpectancyKrw ([double]$row.avg_expectancy_krw) `
            -ProfitableRatio ([double]$row.profitable_ratio) `
            -AvgTotalTrades ([double]$row.avg_total_trades) `
            -MinTradesFloor $MinTradesFloor

        $gateBonus = 0.0
        if ([bool]$row.overall_gate_pass) { $gateBonus += 300.0 }
        if ([bool]$row.profile_gate_pass) { $gateBonus += 80.0 }
        if ([bool]$row.gate_profit_factor_pass) { $gateBonus += 60.0 }
        if ([bool]$row.gate_trades_pass) { $gateBonus += 40.0 }
        $finalObjective = [Math]::Round($objective + $gateBonus, 6)

        $candidates.Add([pscustomobject]@{
            combo_id = $comboId
            combo = $comboById[$comboId]
            objective_score = $objective
            objective_with_gate_bonus = $finalObjective
            avg_profit_factor = [double]$row.avg_profit_factor
            avg_expectancy_krw = [double]$row.avg_expectancy_krw
            profitable_ratio = [double]$row.profitable_ratio
            avg_total_trades = [double]$row.avg_total_trades
            overall_gate_pass = [bool]$row.overall_gate_pass
            profile_gate_pass = [bool]$row.profile_gate_pass
            report_json = [string]$row.report_json
        }) | Out-Null
    }

    if ($candidates.Count -eq 0) {
        throw "No candidate combo rows from tuning summary."
    }

    return @(
        $candidates |
            Sort-Object `
                @{ Expression = { [double]$_.objective_with_gate_bonus }; Descending = $true }, `
                @{ Expression = { [double]$_.avg_expectancy_krw }; Descending = $true }, `
                @{ Expression = { [double]$_.profitable_ratio }; Descending = $true }, `
                @{ Expression = { [double]$_.avg_profit_factor }; Descending = $true }, `
                @{ Expression = { [double]$_.avg_total_trades }; Descending = $true }
    )[0]
}

$resolvedRealDataLoopScript = Resolve-OrThrow -PathValue $RealDataLoopScript -Label "Realdata loop script"
$resolvedTuneScript = Resolve-OrThrow -PathValue $TuneScript -Label "Tune script"
$resolvedBuildConfig = Resolve-OrThrow -PathValue $BuildConfigPath -Label "Build config"
$resolvedSourceConfig = if (Test-Path $SourceConfigPath) { Resolve-OrThrow -PathValue $SourceConfigPath -Label "Source config" } else { $SourceConfigPath }
$resolvedGateReport = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $GateReportJson))
$resolvedTuneSummaryJson = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $TuneSummaryJson))
$resolvedIterationCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $IterationCsv))
$resolvedSummaryJson = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $SummaryJson))

$resolvedLossAnalysisScript = $null
if ($RunLossAnalysis.IsPresent) {
    $resolvedLossAnalysisScript = Resolve-OrThrow -PathValue $LossAnalysisScript -Label "Loss analysis script"
}

Ensure-ParentDirectory -PathValue $resolvedIterationCsv
Ensure-ParentDirectory -PathValue $resolvedSummaryJson

$rows = New-Object "System.Collections.Generic.List[object]"
$startedAt = Get-Date
$status = "running"
$reason = ""
$bestObjective = [double]::NegativeInfinity
$bestSnapshot = $null
$bestComboId = ""
$consecutiveNoImprove = 0

for ($iter = 1; $iter -le $MaxIterations; $iter++) {
    $elapsedMinutes = ((Get-Date) - $startedAt).TotalMinutes
    if ($elapsedMinutes -ge $MaxRuntimeMinutes) {
        $status = "paused_runtime_limit"
        $reason = "Max runtime exceeded before iteration start."
        break
    }

    Write-Host ("[AutoImprove] Iteration {0}/{1} - baseline matrix run" -f $iter, $MaxIterations)
    $realLoopArgs = @{}
    if (-not $FetchEachIteration.IsPresent) {
        $realLoopArgs["SkipFetch"] = $true
    }
    $realLoopArgs["SkipTune"] = $true
    Invoke-ScriptInProcess -ScriptPath $resolvedRealDataLoopScript -ScriptArgs $realLoopArgs -Label "Realdata candidate loop (baseline)"

    if (-not (Test-Path $resolvedGateReport)) {
        throw "Gate report not found after baseline run: $resolvedGateReport"
    }

    $snapshot = Get-CoreSnapshot -ReportPath $resolvedGateReport -MinTradesFloor $MinAvgTrades
    $core = $snapshot.core
    $targetSatisfied = Test-TargetSatisfied `
        -CoreSummary $core `
        -MinPf $MinProfitFactor `
        -MinExp $MinExpectancyKrw `
        -MinRatio $MinProfitableRatio `
        -MinTrades $MinAvgTrades

    $rows.Add([pscustomobject]@{
        iteration = $iter
        phase = "baseline"
        selected_combo = ""
        overall_gate_pass = [bool]$snapshot.overall_gate_pass
        core_vs_legacy_gate_pass = [bool]$snapshot.core_vs_legacy_gate_pass
        core_full_gate_pass = [bool]$core.gate_pass
        avg_profit_factor = [double]$core.avg_profit_factor
        avg_expectancy_krw = [double]$core.avg_expectancy_krw
        avg_total_trades = [double]$core.avg_total_trades
        profitable_ratio = [double]$core.profitable_ratio
        objective_score = [double]$snapshot.objective_score
        target_satisfied = [bool]$targetSatisfied
        timestamp = (Get-Date).ToString("o")
    }) | Out-Null

    if ($snapshot.objective_score -gt ($bestObjective + $ImprovementEpsilon)) {
        $bestObjective = [double]$snapshot.objective_score
        $bestSnapshot = $snapshot
        $bestComboId = ""
        $consecutiveNoImprove = 0
    } else {
        $consecutiveNoImprove++
    }

    if ($targetSatisfied -and [bool]$snapshot.overall_gate_pass) {
        $status = "success_gate_pass"
        $reason = "Target metrics and overall gate passed on baseline run."
        break
    }

    if ($SkipTunePhase.IsPresent) {
        if ($consecutiveNoImprove -ge $MaxConsecutiveNoImprovement) {
            $status = "paused_no_improvement"
            $reason = "No objective improvement within limit while tune phase skipped."
            break
        }
        continue
    }

    $elapsedBeforeTune = ((Get-Date) - $startedAt).TotalMinutes
    if ($elapsedBeforeTune -ge $MaxRuntimeMinutes) {
        $status = "paused_runtime_limit"
        $reason = "Max runtime reached before tune phase."
        break
    }

    Write-Host ("[AutoImprove] Iteration {0}/{1} - tuning combos" -f $iter, $MaxIterations)
    $tuneArgs = @{
        ScenarioMode = $TuneScenarioMode
        MaxScenarios = $TuneMaxScenarios
    }
    if ($TuneIncludeLegacyScenarios.IsPresent) {
        $tuneArgs["IncludeLegacyScenarios"] = $true
    }
    Invoke-ScriptInProcess -ScriptPath $resolvedTuneScript -ScriptArgs $tuneArgs -Label "Candidate tuning"
    if (-not (Test-Path $resolvedTuneSummaryJson)) {
        throw "Tune summary json not found: $resolvedTuneSummaryJson"
    }

    $bestCombo = Select-BestComboFromTuneSummary -TuneSummaryJsonPath $resolvedTuneSummaryJson -MinTradesFloor $MinAvgTrades
    Write-Host ("[AutoImprove] Iteration {0} selected_combo={1} objective={2}" -f $iter, $bestCombo.combo_id, $bestCombo.objective_with_gate_bonus)

    Apply-ComboToConfigFiles `
        -BuildConfig $resolvedBuildConfig `
        -SourceConfig $resolvedSourceConfig `
        -Combo $bestCombo.combo `
        -SyncSource $SyncSourceConfig.IsPresent

    Write-Host ("[AutoImprove] Iteration {0}/{1} - post-apply validation run" -f $iter, $MaxIterations)
    Invoke-ScriptInProcess -ScriptPath $resolvedRealDataLoopScript -ScriptArgs $realLoopArgs -Label "Realdata candidate loop (post-apply)"
    if (-not (Test-Path $resolvedGateReport)) {
        throw "Gate report not found after post-apply run: $resolvedGateReport"
    }

    $postSnapshot = Get-CoreSnapshot -ReportPath $resolvedGateReport -MinTradesFloor $MinAvgTrades
    $postCore = $postSnapshot.core
    $postTargetSatisfied = Test-TargetSatisfied `
        -CoreSummary $postCore `
        -MinPf $MinProfitFactor `
        -MinExp $MinExpectancyKrw `
        -MinRatio $MinProfitableRatio `
        -MinTrades $MinAvgTrades

    $rows.Add([pscustomobject]@{
        iteration = $iter
        phase = "post_apply"
        selected_combo = [string]$bestCombo.combo_id
        overall_gate_pass = [bool]$postSnapshot.overall_gate_pass
        core_vs_legacy_gate_pass = [bool]$postSnapshot.core_vs_legacy_gate_pass
        core_full_gate_pass = [bool]$postCore.gate_pass
        avg_profit_factor = [double]$postCore.avg_profit_factor
        avg_expectancy_krw = [double]$postCore.avg_expectancy_krw
        avg_total_trades = [double]$postCore.avg_total_trades
        profitable_ratio = [double]$postCore.profitable_ratio
        objective_score = [double]$postSnapshot.objective_score
        target_satisfied = [bool]$postTargetSatisfied
        timestamp = (Get-Date).ToString("o")
    }) | Out-Null

    if ($postSnapshot.objective_score -gt ($bestObjective + $ImprovementEpsilon)) {
        $bestObjective = [double]$postSnapshot.objective_score
        $bestSnapshot = $postSnapshot
        $bestComboId = [string]$bestCombo.combo_id
        $consecutiveNoImprove = 0
    } else {
        $consecutiveNoImprove++
    }

    if ($RunLossAnalysis.IsPresent -and $null -ne $resolvedLossAnalysisScript) {
        Write-Host ("[AutoImprove] Iteration {0}/{1} - loss contributor analysis" -f $iter, $MaxIterations)
        Invoke-PsFile -ScriptPath $resolvedLossAnalysisScript -Args @() -Label "Loss contributor analysis"
    }

    if ($postTargetSatisfied -and [bool]$postSnapshot.overall_gate_pass) {
        $status = "success_gate_pass"
        $reason = "Target metrics and overall gate passed on post-apply run."
        break
    }

    if ($consecutiveNoImprove -ge $MaxConsecutiveNoImprovement) {
        $status = "paused_no_improvement"
        $reason = "Objective score did not improve within configured consecutive limit."
        break
    }
}

if ($status -eq "running") {
    if ($MaxIterations -gt 0 -and $rows.Count -gt 0) {
        $status = "paused_max_iterations"
        $reason = "Reached MaxIterations without full gate pass."
    } else {
        $status = "paused_no_data"
        $reason = "No iteration rows produced."
    }
}

$rowsArray = @($rows.ToArray())
$rowsArray | Export-Csv -Path $resolvedIterationCsv -NoTypeInformation -Encoding UTF8

$summary = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    status = $status
    reason = $reason
    started_at = $startedAt.ToString("o")
    ended_at = (Get-Date).ToString("o")
    max_iterations = $MaxIterations
    max_runtime_minutes = $MaxRuntimeMinutes
    max_consecutive_no_improvement = $MaxConsecutiveNoImprovement
    tuning = [ordered]@{
        scenario_mode = $TuneScenarioMode
        max_scenarios = $TuneMaxScenarios
        include_legacy = [bool]$TuneIncludeLegacyScenarios
    }
    targets = [ordered]@{
        min_profit_factor = $MinProfitFactor
        min_expectancy_krw = $MinExpectancyKrw
        min_profitable_ratio = $MinProfitableRatio
        min_avg_trades = $MinAvgTrades
    }
    best_objective_score = $bestObjective
    best_combo_id = $bestComboId
    best_snapshot = if ($null -ne $bestSnapshot) {
        [ordered]@{
            overall_gate_pass = [bool]$bestSnapshot.overall_gate_pass
            core_vs_legacy_gate_pass = [bool]$bestSnapshot.core_vs_legacy_gate_pass
            avg_profit_factor = [double]$bestSnapshot.core.avg_profit_factor
            avg_expectancy_krw = [double]$bestSnapshot.core.avg_expectancy_krw
            avg_total_trades = [double]$bestSnapshot.core.avg_total_trades
            profitable_ratio = [double]$bestSnapshot.core.profitable_ratio
            core_full_gate_pass = [bool]$bestSnapshot.core.gate_pass
        }
    } else {
        $null
    }
    outputs = [ordered]@{
        iteration_csv = $resolvedIterationCsv
        summary_json = $resolvedSummaryJson
        gate_report_json = $resolvedGateReport
        tune_summary_json = $resolvedTuneSummaryJson
    }
    iterations = $rowsArray
}

($summary | ConvertTo-Json -Depth 10) | Set-Content -Path $resolvedSummaryJson -Encoding UTF8

Write-Host "[AutoImprove] Completed"
Write-Host ("status={0}" -f $status)
Write-Host ("reason={0}" -f $reason)
Write-Host ("iteration_csv={0}" -f $resolvedIterationCsv)
Write-Host ("summary_json={0}" -f $resolvedSummaryJson)
if ($null -ne $bestSnapshot) {
    Write-Host ("best_objective={0}" -f $bestObjective)
    Write-Host ("best_combo_id={0}" -f $bestComboId)
    Write-Host ("best_avg_profit_factor={0}" -f $bestSnapshot.core.avg_profit_factor)
    Write-Host ("best_avg_expectancy_krw={0}" -f $bestSnapshot.core.avg_expectancy_krw)
    Write-Host ("best_avg_total_trades={0}" -f $bestSnapshot.core.avg_total_trades)
    Write-Host ("best_profitable_ratio={0}" -f $bestSnapshot.core.profitable_ratio)
}

exit 0
