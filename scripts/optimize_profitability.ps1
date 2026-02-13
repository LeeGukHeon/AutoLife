param(
    [string]$ExePath = ".\\build\\Release\\AutoLifeTrading.exe",
    [string]$ConfigPath = ".\\build\\Release\\config\\config.json",
    [string]$SourceConfigPath = ".\\config\\config.json",
    [string]$DatasetA = ".\\data\\backtest\\simulation_2000.csv",
    [string]$DatasetB = ".\\data\\backtest\\simulation_large.csv",
    [string]$OutputCsv = ".\\build\\Release\\logs\\optimization_grid.csv",
    [string]$OutputJson = ".\\build\\Release\\logs\\optimization_best.json",
    [string]$WalkForwardScript = ".\\scripts\\walk_forward_validate.ps1",
    [int]$TopKWalkForward = 6,
    [int]$PerRunDelayMs = 120,
    [switch]$UseWalkForward,
    [switch]$ApplyBest
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $ExePath)) { throw "Executable not found: $ExePath" }
if (!(Test-Path $ConfigPath)) { throw "Config not found: $ConfigPath" }
if (!(Test-Path $DatasetA)) { throw "Dataset not found: $DatasetA" }
if (!(Test-Path $DatasetB)) { throw "Dataset not found: $DatasetB" }
if (!(Test-Path $WalkForwardScript)) { throw "Walk-forward script not found: $WalkForwardScript" }

$exeResolved = Resolve-Path $ExePath
$exeDir = Split-Path -Parent $exeResolved
$configResolved = Resolve-Path $ConfigPath
$datasetAResolved = Resolve-Path $DatasetA
$datasetBResolved = Resolve-Path $DatasetB
$walkForwardResolved = Resolve-Path $WalkForwardScript
$exeLogDir = Join-Path $exeDir "logs"
if (Test-Path $exeLogDir) {
    Get-ChildItem -Path $exeLogDir -Filter "autolife*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {}
    }
}

function Invoke-BacktestJson([string]$exe, [string]$csvPath) {
    $csvResolved = (Resolve-Path $csvPath).Path
    Push-Location $exeDir
    try {
        $out = & $exe --backtest $csvResolved --json 2>&1
    } finally {
        Pop-Location
    }
    $text = ($out | Out-String)
    $jsonLine = $null
    $text -split "`r?`n" | ForEach-Object {
        $line = $_.Trim()
        if ($line.StartsWith("{") -and $line.EndsWith("}")) {
            $jsonLine = $line
        }
    }
    if (-not $jsonLine) { return $null }
    try {
        return ($jsonLine | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Compute-Score($a, $b) {
    if ($null -eq $a -or $null -eq $b) { return -1e15 }

    $pA = [double]$a.total_profit
    $pB = [double]$b.total_profit
    $pfA = [double]$a.profit_factor
    $pfB = [double]$b.profit_factor
    $expA = [double]$a.expectancy_krw
    $expB = [double]$b.expectancy_krw
    $mddA = [double]$a.max_drawdown * 100.0
    $mddB = [double]$b.max_drawdown * 100.0
    $tA = [int]$a.total_trades
    $tB = [int]$b.total_trades

    $score = 0.0
    $score += ($pA + $pB)
    $score += 180.0 * ($expA + $expB)
    $score += 1100.0 * (($pfA - 1.0) + ($pfB - 1.0))
    $score -= 260.0 * ([math]::Max(0.0, $mddA - 11.5) + [math]::Max(0.0, $mddB - 11.5))
    if ($tA -lt 25 -or $tB -lt 25) { $score -= 850.0 }
    if ($tA -lt 10 -or $tB -lt 10) { $score -= 1400.0 }
    if ($pA -le 0.0 -or $pB -le 0.0) { $score -= 500.0 }
    return [math]::Round($score, 4)
}

function Invoke-WalkForwardScore([string]$csvPath) {
    if (!(Test-Path $walkForwardResolved)) { return $null }
    $tmpWfJson = Join-Path $exeLogDir "optimization_wf_tmp.json"
    $csvResolved = (Resolve-Path $csvPath).Path
    Push-Location $exeDir
    try {
        $null = & $walkForwardResolved `
        -ExePath $exeResolved `
        -InputCsv $csvResolved `
        -OutputJson $tmpWfJson 2>&1
    } finally {
        Pop-Location
    }
    if (!(Test-Path $tmpWfJson)) { return $null }
    try {
        return (Get-Content -Path $tmpWfJson -Encoding UTF8 | Out-String | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Set-OrAddProperty($obj, [string]$name, $value) {
    if ($null -eq $obj) { return }
    $prop = $obj.PSObject.Properties[$name]
    if ($null -eq $prop) {
        $obj | Add-Member -NotePropertyName $name -NotePropertyValue $value
    } else {
        $obj.$name = $value
    }
}

$cfgRaw = Get-Content -Path $configResolved -Encoding UTF8 | Out-String
$cfg = $cfgRaw | ConvertFrom-Json
if ($null -eq $cfg.trading) {
    $cfg | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
}
if ($null -eq $cfg.strategies) {
    $cfg | Add-Member -NotePropertyName strategies -NotePropertyValue ([pscustomobject]@{})
}
if ($null -eq $cfg.strategies.scalping) {
    $cfg.strategies | Add-Member -NotePropertyName scalping -NotePropertyValue ([pscustomobject]@{})
}
if ($null -eq $cfg.strategies.momentum) {
    $cfg.strategies | Add-Member -NotePropertyName momentum -NotePropertyValue ([pscustomobject]@{})
}

$weakRrGrid = @(1.80)
$strongRrGrid = @(1.20)
$edgeGrid = @(0.0012)
$evPfGrid = @(0.95)
$evTradesGrid = @(30)
$evExpGrid = @(-2.0)
$scalpMinStrengthGrid = @(0.66, 0.70, 0.74)
$momMinStrengthGrid = @(0.60, 0.64, 0.68, 0.72)

$rows = @()
$comboIndex = 0
$comboTotal = ($weakRrGrid.Count * $strongRrGrid.Count * $edgeGrid.Count * $evPfGrid.Count * $evTradesGrid.Count * $evExpGrid.Count * $scalpMinStrengthGrid.Count * $momMinStrengthGrid.Count)

try {
    foreach ($weak in $weakRrGrid) {
        foreach ($strong in $strongRrGrid) {
            if ($strong -gt $weak) { continue }
            foreach ($edge in $edgeGrid) {
                foreach ($evPf in $evPfGrid) {
                    foreach ($evTrades in $evTradesGrid) {
                        foreach ($evExp in $evExpGrid) {
                            foreach ($scalpMinStrength in $scalpMinStrengthGrid) {
                                foreach ($momMinStrength in $momMinStrengthGrid) {
                                    $comboIndex++
                                    Write-Output ("[{0}/{1}] weakRR={2} strongRR={3} edge={4} evPF={5} evTrades={6} evExp={7} scalpingMin={8} momentumMin={9}" -f
                                        $comboIndex, $comboTotal, $weak, $strong, $edge, $evPf, $evTrades, $evExp, $scalpMinStrength, $momMinStrength)

                                    $cfg.trading.min_rr_weak_signal = $weak
                                    $cfg.trading.min_rr_strong_signal = $strong
                                    $cfg.trading.min_expected_edge_pct = $edge
                                    $cfg.trading.min_strategy_profit_factor = $evPf
                                    $cfg.trading.min_strategy_trades_for_ev = $evTrades
                                    $cfg.trading.min_strategy_expectancy_krw = $evExp
                                    Set-OrAddProperty -obj $cfg.strategies.scalping -name "min_signal_strength" -value $scalpMinStrength
                                    Set-OrAddProperty -obj $cfg.strategies.momentum -name "min_signal_strength" -value $momMinStrength

                                    $cfg | ConvertTo-Json -Depth 32 | Set-Content -Path $configResolved -Encoding UTF8

                                    $a = Invoke-BacktestJson -exe $exeResolved -csvPath $datasetAResolved
                                    $b = Invoke-BacktestJson -exe $exeResolved -csvPath $datasetBResolved
                                    if ($PerRunDelayMs -gt 0) {
                                        Start-Sleep -Milliseconds $PerRunDelayMs
                                    }
                                    $score = Compute-Score -a $a -b $b

                                    $rows += [pscustomobject]@{
                                        weak_rr = $weak
                                        strong_rr = $strong
                                        min_expected_edge_pct = $edge
                                        min_strategy_profit_factor = $evPf
                                        min_strategy_trades_for_ev = $evTrades
                                        min_strategy_expectancy_krw = $evExp
                                        scalping_min_signal_strength = $scalpMinStrength
                                        momentum_min_signal_strength = $momMinStrength
                                        score = $score
                                        final_score = $score
                                        wf_score = $null
                                        wf_ratio = $null
                                        wf_profit = $null
                                        wf_mdd_pct = $null
                                        profit_a = if ($a) { [double]$a.total_profit } else { $null }
                                        pf_a = if ($a) { [double]$a.profit_factor } else { $null }
                                        exp_a = if ($a) { [double]$a.expectancy_krw } else { $null }
                                        mdd_a_pct = if ($a) { [double]$a.max_drawdown * 100.0 } else { $null }
                                        trades_a = if ($a) { [int]$a.total_trades } else { $null }
                                        profit_b = if ($b) { [double]$b.total_profit } else { $null }
                                        pf_b = if ($b) { [double]$b.profit_factor } else { $null }
                                        exp_b = if ($b) { [double]$b.expectancy_krw } else { $null }
                                        mdd_b_pct = if ($b) { [double]$b.max_drawdown * 100.0 } else { $null }
                                        trades_b = if ($b) { [int]$b.total_trades } else { $null }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
finally {
    $cfgRaw | Set-Content -Path $configResolved -Encoding UTF8
}

if ($rows.Count -eq 0) {
    throw "No optimization rows generated."
}

$rowsSorted = @($rows | Sort-Object score -Descending)

if ($UseWalkForward) {
    $topK = [Math]::Min($TopKWalkForward, $rowsSorted.Count)
    for ($i = 0; $i -lt $topK; $i++) {
        $cand = $rowsSorted[$i]
        $cfg.trading.min_rr_weak_signal = [double]$cand.weak_rr
        $cfg.trading.min_rr_strong_signal = [double]$cand.strong_rr
        $cfg.trading.min_expected_edge_pct = [double]$cand.min_expected_edge_pct
        $cfg.trading.min_strategy_profit_factor = [double]$cand.min_strategy_profit_factor
        $cfg.trading.min_strategy_trades_for_ev = [int]$cand.min_strategy_trades_for_ev
        $cfg.trading.min_strategy_expectancy_krw = [double]$cand.min_strategy_expectancy_krw
        Set-OrAddProperty -obj $cfg.strategies.scalping -name "min_signal_strength" -value ([double]$cand.scalping_min_signal_strength)
        Set-OrAddProperty -obj $cfg.strategies.momentum -name "min_signal_strength" -value ([double]$cand.momentum_min_signal_strength)
        $cfg | ConvertTo-Json -Depth 32 | Set-Content -Path $configResolved -Encoding UTF8

        $wf = Invoke-WalkForwardScore -csvPath $datasetAResolved
        if ($wf) {
            $ratio = [double]$wf.oos_profitable_ratio
            $profit = [double]$wf.oos_total_profit
            $mdd = [double]$wf.oos_max_mdd_pct
            $readyBonus = if ([bool]$wf.is_ready_for_live_walkforward) { 350.0 } else { 0.0 }
            $wfScore = ($profit * 0.08) + (600.0 * $ratio) - (180.0 * [math]::Max(0.0, $mdd - 12.0)) + $readyBonus
            $cand.wf_score = [math]::Round($wfScore, 4)
            $cand.wf_ratio = $ratio
            $cand.wf_profit = $profit
            $cand.wf_mdd_pct = $mdd
            $cand.final_score = [math]::Round(([double]$cand.score + $wfScore), 4)
        }
    }
}

$rowsSorted = @($rows | Sort-Object final_score -Descending)
$rowsSorted | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputCsv
$best = $rowsSorted[0]

$bestReport = [pscustomobject]@{
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    best = $best
    top10 = @($rowsSorted | Select-Object -First 10)
}
$bestReport | ConvertTo-Json -Depth 6 | Set-Content -Path $OutputJson -Encoding UTF8

Write-Output "=== Optimization Top 10 ==="
$rowsSorted | Select-Object -First 10 | Format-Table -AutoSize | Out-String -Width 260 | Write-Output
Write-Output "saved_csv=$OutputCsv"
Write-Output "saved_json=$OutputJson"

if ($ApplyBest) {
    $cfgApply = (Get-Content -Path $configResolved -Encoding UTF8 | Out-String | ConvertFrom-Json)
    if ($null -eq $cfgApply.trading) {
        $cfgApply | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
    }
    $cfgApply.trading.min_rr_weak_signal = [double]$best.weak_rr
    $cfgApply.trading.min_rr_strong_signal = [double]$best.strong_rr
    $cfgApply.trading.min_expected_edge_pct = [double]$best.min_expected_edge_pct
    $cfgApply.trading.min_strategy_profit_factor = [double]$best.min_strategy_profit_factor
    $cfgApply.trading.min_strategy_trades_for_ev = [int]$best.min_strategy_trades_for_ev
    $cfgApply.trading.min_strategy_expectancy_krw = [double]$best.min_strategy_expectancy_krw
    if ($null -eq $cfgApply.strategies) {
        $cfgApply | Add-Member -NotePropertyName strategies -NotePropertyValue ([pscustomobject]@{})
    }
    if ($null -eq $cfgApply.strategies.scalping) {
        $cfgApply.strategies | Add-Member -NotePropertyName scalping -NotePropertyValue ([pscustomobject]@{})
    }
    if ($null -eq $cfgApply.strategies.momentum) {
        $cfgApply.strategies | Add-Member -NotePropertyName momentum -NotePropertyValue ([pscustomobject]@{})
    }
    Set-OrAddProperty -obj $cfgApply.strategies.scalping -name "min_signal_strength" -value ([double]$best.scalping_min_signal_strength)
    Set-OrAddProperty -obj $cfgApply.strategies.momentum -name "min_signal_strength" -value ([double]$best.momentum_min_signal_strength)
    $cfgApply | ConvertTo-Json -Depth 32 | Set-Content -Path $configResolved -Encoding UTF8
    Write-Output "Applied best parameters to $configResolved"
    if (Test-Path $SourceConfigPath) {
        $cfgApply | ConvertTo-Json -Depth 32 | Set-Content -Path $SourceConfigPath -Encoding UTF8
        Write-Output "Synced best parameters to $SourceConfigPath"
    }
}
