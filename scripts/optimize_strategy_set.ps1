param(
    [string]$ExePath = ".\\build\\Release\\AutoLifeTrading.exe",
    [string]$ConfigPath = ".\\build\\Release\\config\\config.json",
    [string]$SourceConfigPath = ".\\config\\config.json",
    [string]$DatasetA = ".\\data\\backtest\\simulation_2000.csv",
    [string]$DatasetB = ".\\data\\backtest\\simulation_large.csv",
    [string]$OutputCsv = ".\\build\\Release\\logs\\strategy_set_grid.csv",
    [string]$OutputJson = ".\\build\\Release\\logs\\strategy_set_best.json",
    [switch]$ApplyBest
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $ExePath)) { throw "Executable not found: $ExePath" }
if (!(Test-Path $ConfigPath)) { throw "Config not found: $ConfigPath" }
if (!(Test-Path $DatasetA)) { throw "Dataset not found: $DatasetA" }
if (!(Test-Path $DatasetB)) { throw "Dataset not found: $DatasetB" }

function Invoke-BacktestJson([string]$exe, [string]$csvPath) {
    $out = & $exe --backtest $csvPath --json 2>&1
    $text = ($out | Out-String)
    $jsonLine = $null
    $text -split "`r?`n" | ForEach-Object {
        $line = $_.Trim()
        if ($line.StartsWith("{") -and $line.EndsWith("}")) {
            $jsonLine = $line
        }
    }
    if (-not $jsonLine) { return $null }
    try { return ($jsonLine | ConvertFrom-Json) } catch { return $null }
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
    $score += 250.0 * ($expA + $expB)
    $score += 900.0 * (($pfA - 1.0) + ($pfB - 1.0))
    $score -= 220.0 * ([math]::Max(0.0, $mddA - 12.0) + [math]::Max(0.0, $mddB - 12.0))
    if ($pA -le 0.0 -or $pB -le 0.0) { $score -= 400.0 }
    if ($tA -lt 30 -or $tB -lt 30) { $score -= 100000.0 }
    return [math]::Round($score, 4)
}

$allStrategies = @("scalping", "momentum", "breakout", "mean_reversion", "grid")
$cfgRaw = Get-Content -Path $ConfigPath -Encoding UTF8 | Out-String
$cfg = $cfgRaw | ConvertFrom-Json
if ($null -eq $cfg.trading) {
    $cfg | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
}

$rows = @()
$sets = @()
for ($mask = 1; $mask -lt [math]::Pow(2, $allStrategies.Count); $mask++) {
    $set = @()
    for ($i = 0; $i -lt $allStrategies.Count; $i++) {
        if (($mask -band (1 -shl $i)) -ne 0) {
            $set += $allStrategies[$i]
        }
    }
    $sets += ,$set
}

$idx = 0
try {
    foreach ($set in $sets) {
        $idx++
        $setText = ($set -join ",")
        Write-Output ("[{0}/{1}] enabled={2}" -f $idx, $sets.Count, $setText)
        $cfg.trading.enabled_strategies = $set
        $cfg | ConvertTo-Json -Depth 32 | Set-Content -Path $ConfigPath -Encoding UTF8

        $a = Invoke-BacktestJson -exe $ExePath -csvPath $DatasetA
        $b = Invoke-BacktestJson -exe $ExePath -csvPath $DatasetB
        $score = Compute-Score -a $a -b $b

        $rows += [pscustomobject]@{
            enabled_strategies = $setText
            strategy_count = $set.Count
            score = $score
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
finally {
    $cfgRaw | Set-Content -Path $ConfigPath -Encoding UTF8
}

if ($rows.Count -eq 0) {
    throw "No strategy set rows generated."
}

$sorted = @($rows | Sort-Object score -Descending)
$sorted | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputCsv
$best = $sorted[0]

$report = [pscustomobject]@{
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    best = $best
    top10 = @($sorted | Select-Object -First 10)
}
$report | ConvertTo-Json -Depth 6 | Set-Content -Path $OutputJson -Encoding UTF8

Write-Output "=== Strategy Set Top 10 ==="
$sorted | Select-Object -First 10 | Format-Table -AutoSize | Out-String -Width 260 | Write-Output
Write-Output "saved_csv=$OutputCsv"
Write-Output "saved_json=$OutputJson"

if ($ApplyBest) {
    $cfgApply = (Get-Content -Path $ConfigPath -Encoding UTF8 | Out-String | ConvertFrom-Json)
    if ($null -eq $cfgApply.trading) {
        $cfgApply | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
    }
    $bestSet = @($best.enabled_strategies -split "," | Where-Object { $_ -ne "" })
    $cfgApply.trading.enabled_strategies = $bestSet
    $cfgApply | ConvertTo-Json -Depth 32 | Set-Content -Path $ConfigPath -Encoding UTF8
    Write-Output ("Applied best strategy set to {0}: {1}" -f $ConfigPath, ($bestSet -join ","))
    if (Test-Path $SourceConfigPath) {
        $cfgApply | ConvertTo-Json -Depth 32 | Set-Content -Path $SourceConfigPath -Encoding UTF8
        Write-Output ("Synced strategy set to {0}: {1}" -f $SourceConfigPath, ($bestSet -join ","))
    }
}
