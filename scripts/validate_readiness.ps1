param(
    [string]$ExePath = ".\\build\\Release\\AutoLifeTrading.exe",
    [string]$DataDir = ".\\data\\backtest",
    [string]$OutputCsv = ".\\build\\Release\\logs\\backtest_matrix_summary.csv",
    [string]$OutputStrategyCsv = ".\\build\\Release\\logs\\backtest_strategy_summary.csv",
    [string]$OutputProfileCsv = ".\\build\\Release\\logs\\backtest_profile_summary.csv",
    [string]$OutputJson = ".\\build\\Release\\logs\\readiness_report.json",
    [switch]$Recurse,
    [int]$MinTrades = 30,
    [switch]$IncludeStrategyMatrix
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $ExePath)) {
    throw "Executable not found: $ExePath"
}
if (!(Test-Path $DataDir)) {
    throw "Backtest data dir not found: $DataDir"
}

$files = Get-ChildItem -Path $DataDir -File -Filter *.csv -Recurse:$Recurse | Sort-Object FullName
if ($files.Count -eq 0) {
    throw "No CSV files found in $DataDir"
}

if (-not $IncludeStrategyMatrix) {
    $IncludeStrategyMatrix = $true
}

$profiles = @(
    [pscustomobject]@{ name = "all"; strategies = @() }
)
if ($IncludeStrategyMatrix) {
    $profiles += @(
        [pscustomobject]@{ name = "scalping_only"; strategies = @("scalping") },
        [pscustomobject]@{ name = "momentum_only"; strategies = @("momentum") },
        [pscustomobject]@{ name = "breakout_only"; strategies = @("breakout") },
        [pscustomobject]@{ name = "mean_reversion_only"; strategies = @("mean_reversion") },
        [pscustomobject]@{ name = "grid_only"; strategies = @("grid_trading") },
        [pscustomobject]@{ name = "scalp_momentum"; strategies = @("scalping", "momentum") },
        [pscustomobject]@{ name = "scalp_breakout"; strategies = @("scalping", "breakout") },
        [pscustomobject]@{ name = "scalp_meanrev"; strategies = @("scalping", "mean_reversion") },
        [pscustomobject]@{ name = "trend_pair"; strategies = @("momentum", "breakout") },
        [pscustomobject]@{ name = "range_pair"; strategies = @("mean_reversion", "grid_trading") }
    )
}

$rows = @()
$strategyRows = @()
foreach ($profile in $profiles) {
    $strategyCsv = ""
    if ($profile.strategies.Count -gt 0) {
        $strategyCsv = ($profile.strategies -join ",")
    }

    foreach ($f in $files) {
        $args = @("--backtest", $f.FullName, "--json")
        if ($strategyCsv) {
            $args += @("--strategies", $strategyCsv)
        }

        $out = & $ExePath @args 2>&1
        $text = ($out | Out-String)

        $final = $null
        $profit = $null
        $mdd = $null
        $trades = $null
        $wins = $null

        $jsonLine = $null
        $text -split "`r?`n" | ForEach-Object {
            $line = $_.Trim()
            if ($line.StartsWith("{") -and $line.EndsWith("}")) {
                $jsonLine = $line
            }
        }
        if ($jsonLine) {
            try {
                $j = $jsonLine | ConvertFrom-Json
                $final = [double]$j.final_balance
                $profit = [double]$j.total_profit
                $mdd = [double]$j.max_drawdown * 100.0
                $trades = [int]$j.total_trades
                $wins = [int]$j.winning_trades

                if ($j.PSObject.Properties.Name -contains "strategy_summaries" -and $j.strategy_summaries) {
                    foreach ($s in $j.strategy_summaries) {
                        $strategyRows += [pscustomobject]@{
                            profile_name   = $profile.name
                            strategies_csv = $strategyCsv
                            dataset_file   = $f.Name
                            strategy_name  = [string]$s.strategy_name
                            total_trades   = [int]$s.total_trades
                            winning_trades = [int]$s.winning_trades
                            losing_trades  = [int]$s.losing_trades
                            win_rate_pct   = [double]$s.win_rate * 100.0
                            total_profit   = [double]$s.total_profit
                            avg_win_krw    = [double]$s.avg_win_krw
                            avg_loss_krw   = [double]$s.avg_loss_krw
                            profit_factor  = [double]$s.profit_factor
                        }
                    }
                }
            } catch {
                # fallback left as nulls
            }
        }

        $winRate = $null
        if ($trades -and $trades -gt 0 -and $wins -ne $null) {
            $winRate = [math]::Round(($wins / $trades) * 100.0, 2)
        }

        $resolvedRoot = (Resolve-Path $DataDir).Path
        $relativePath = $f.FullName
        if ($relativePath.StartsWith($resolvedRoot)) {
            $relativePath = $relativePath.Substring($resolvedRoot.Length).TrimStart('\', '/')
        }

        $rows += [pscustomobject]@{
            profile_name   = $profile.name
            strategies_csv = $strategyCsv
            file           = $f.Name
            relative_path  = $relativePath
            final_balance  = $final
            total_profit   = $profit
            mdd_pct        = $mdd
            total_trades   = $trades
            winning_trades = $wins
            win_rate_pct   = $winRate
        }
    }
}

$rows | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputCsv

$strategySummary = @()
if ($strategyRows.Count -gt 0) {
    $strategySummary = @(
        $strategyRows |
            Group-Object profile_name, strategy_name |
            ForEach-Object {
                $g = $_.Group
                $sumTrades = ($g | Measure-Object -Property total_trades -Sum).Sum
                $sumWins = ($g | Measure-Object -Property winning_trades -Sum).Sum
                $sumLosses = ($g | Measure-Object -Property losing_trades -Sum).Sum
                $sumProfit = ($g | Measure-Object -Property total_profit -Sum).Sum
                $winRatePct = if ($sumTrades -gt 0) { [math]::Round(($sumWins / $sumTrades) * 100.0, 2) } else { 0.0 }

                [pscustomobject]@{
                    profile_name   = $g[0].profile_name
                    strategy_name  = $g[0].strategy_name
                    total_trades   = [int]$sumTrades
                    winning_trades = [int]$sumWins
                    losing_trades  = [int]$sumLosses
                    win_rate_pct   = $winRatePct
                    total_profit   = [double]$sumProfit
                }
            } |
            Sort-Object profile_name, total_profit -Descending
    )
}

$strategySummary | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputStrategyCsv

$profileSummary = @(
    $rows |
        Group-Object profile_name |
        ForEach-Object {
            $name = $_.Name
            $groupRows = @($_.Group)
            $evaluated = @($groupRows | Where-Object { $_.total_trades -ge $MinTrades })
            $profitable = @($evaluated | Where-Object { $_.total_profit -gt 0 })
            $strict = @(
                $evaluated | Where-Object {
                    $_.total_profit -gt 0 -and
                    $_.mdd_pct -le 10 -and
                    $_.win_rate_pct -ge 55
                }
            )
            $profitRatio = 0.0
            if ($evaluated.Count -gt 0) {
                $profitRatio = [math]::Round($profitable.Count / $evaluated.Count, 4)
            }
            [pscustomobject]@{
                profile_name             = $name
                dataset_total            = $groupRows.Count
                dataset_evaluated        = $evaluated.Count
                profitable_datasets      = $profitable.Count
                strict_pass_datasets     = $strict.Count
                profitable_ratio         = $profitRatio
                is_ready_for_live_profile = (($profitRatio -ge 0.60) -and ($strict.Count -ge 2))
            }
        } |
        Sort-Object profile_name
)
$profileSummary | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputProfileCsv

$primaryRows = @($rows | Where-Object { $_.profile_name -eq "all" })
$evaluated = @($primaryRows | Where-Object { $_.total_trades -ge $MinTrades })
$profitable = @($evaluated | Where-Object { $_.total_profit -gt 0 })
$strict = @(
    $evaluated | Where-Object {
        $_.total_profit -gt 0 -and
        $_.mdd_pct -le 10 -and
        $_.win_rate_pct -ge 55
    }
)
$profitRatio = 0.0
if ($evaluated.Count -gt 0) {
    $profitRatio = [math]::Round($profitable.Count / $evaluated.Count, 4)
}
$isReady = ($profitRatio -ge 0.60) -and ($strict.Count -ge 2)

$report = [pscustomobject]@{
    generated_at_utc              = (Get-Date).ToUniversalTime().ToString("o")
    dataset_total                 = $primaryRows.Count
    dataset_evaluated             = $evaluated.Count
    min_trades_threshold          = $MinTrades
    recursive_scan                = [bool]$Recurse
    profitable_datasets           = $profitable.Count
    strict_pass_datasets          = $strict.Count
    profitable_ratio              = $profitRatio
    readiness_gate_profitable     = ">= 0.60"
    readiness_gate_strict_pass    = ">= 2"
    is_ready_for_live_by_backtest = $isReady
    strategy_summary              = $strategySummary
    profile_summary               = $profileSummary
}

$report | ConvertTo-Json -Depth 6 | Set-Content -Path $OutputJson -Encoding UTF8

Write-Output "=== Backtest Matrix Summary ==="
$rows | Format-Table -AutoSize | Out-String -Width 220 | Write-Output
Write-Output "=== Readiness Verdict ==="
$report | Format-List | Out-String | Write-Output
Write-Output "saved_csv=$OutputCsv"
Write-Output "saved_strategy_csv=$OutputStrategyCsv"
Write-Output "saved_profile_csv=$OutputProfileCsv"
Write-Output "saved_json=$OutputJson"
