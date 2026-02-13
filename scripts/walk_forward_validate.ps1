param(
    [string]$ExePath = ".\\build\\Release\\AutoLifeTrading.exe",
    [string]$InputCsv = ".\\data\\backtest\\simulation_2000.csv",
    [int]$TrainSize = 600,
    [int]$TestSize = 200,
    [int]$StepSize = 200,
    [int]$MinTrainTrades = 30,
    [string]$OutputCsv = ".\\build\\Release\\logs\\walk_forward_windows.csv",
    [bool]$RunAllTests = $true,
    [string]$OutputJson = ".\\build\\Release\\logs\\walk_forward_report.json"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $ExePath)) { throw "Executable not found: $ExePath" }
if (!(Test-Path $InputCsv)) { throw "Input CSV not found: $InputCsv" }

$all = Get-Content $InputCsv
if ($all.Count -lt 2) { throw "CSV has no data rows: $InputCsv" }

$header = $all[0]
$rows = @($all[1..($all.Count - 1)])
$n = $rows.Count

$tmpDir = ".\\build\\Release\\logs\\walk_forward_tmp"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

function Invoke-BacktestJson([string]$csvPath) {
    $out = & $ExePath --backtest $csvPath --json 2>&1
    $text = ($out | Out-String)
    $jsonLine = $null
    $text -split "`r?`n" | ForEach-Object {
        $line = $_.Trim()
        if ($line.StartsWith("{") -and $line.EndsWith("}")) { $jsonLine = $line }
    }
    if (-not $jsonLine) {
        return $null
    }
    return ($jsonLine | ConvertFrom-Json)
}

function New-SliceCsv([string]$path, [string]$headerLine, [object[]]$sliceRows) {
    $content = @($headerLine) + $sliceRows
    Set-Content -Path $path -Encoding UTF8 -Value $content
}

$windows = @()
$windowId = 0

for ($start = 0; $start + $TrainSize + $TestSize -le $n; $start += $StepSize) {
    $windowId++
    $trainStart = $start
    $trainEnd = $start + $TrainSize - 1
    $testStart = $trainEnd + 1
    $testEnd = $testStart + $TestSize - 1

    $trainRows = @($rows[$trainStart..$trainEnd])
    $testRows = @($rows[$testStart..$testEnd])

    $trainCsv = Join-Path $tmpDir ("wf_train_{0}.csv" -f $windowId)
    $testCsv = Join-Path $tmpDir ("wf_test_{0}.csv" -f $windowId)
    New-SliceCsv -path $trainCsv -headerLine $header -sliceRows $trainRows
    New-SliceCsv -path $testCsv -headerLine $header -sliceRows $testRows

    $train = Invoke-BacktestJson -csvPath $trainCsv
    if (-not $train) { continue }

    $trainTrades = [int]$train.total_trades
    $trainWins = [int]$train.winning_trades
    $trainWinRate = if ($trainTrades -gt 0) { [math]::Round(($trainWins / $trainTrades) * 100.0, 2) } else { 0.0 }
    $trainMddPct = [double]$train.max_drawdown * 100.0
    $trainProfit = [double]$train.total_profit

    $trainPass = (
        ($trainTrades -ge $MinTrainTrades) -and
        ($trainProfit -gt 0.0) -and
        ($trainMddPct -le 10.0) -and
        ($trainWinRate -ge 55.0)
    )

    $testProfit = 0.0
    $testMddPct = 0.0
    $testTrades = 0
    $testWins = 0
    $testWinRate = 0.0
    $testRan = $false

    if ($trainPass -or $RunAllTests) {
        $test = Invoke-BacktestJson -csvPath $testCsv
        if ($test) {
            $testRan = $true
            $testProfit = [double]$test.total_profit
            $testMddPct = [double]$test.max_drawdown * 100.0
            $testTrades = [int]$test.total_trades
            $testWins = [int]$test.winning_trades
            $testWinRate = if ($testTrades -gt 0) { [math]::Round(($testWins / $testTrades) * 100.0, 2) } else { 0.0 }
        }
    }

    $windows += [pscustomobject]@{
        window_id        = $windowId
        train_start      = $trainStart
        train_end        = $trainEnd
        test_start       = $testStart
        test_end         = $testEnd
        train_trades     = $trainTrades
        train_win_rate   = $trainWinRate
        train_profit     = [math]::Round($trainProfit, 4)
        train_mdd_pct    = [math]::Round($trainMddPct, 4)
        train_pass       = $trainPass
        test_ran         = $testRan
        test_trades      = $testTrades
        test_win_rate    = $testWinRate
        test_profit      = [math]::Round($testProfit, 4)
        test_mdd_pct     = [math]::Round($testMddPct, 4)
        test_profitable  = ($testProfit -gt 0.0)
    }
}

$windows | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputCsv

$ran = @($windows | Where-Object { $_.test_ran -eq $true })
$ranCount = $ran.Count
$profitableCount = @($ran | Where-Object { $_.test_profitable -eq $true }).Count
$profitRatio = if ($ranCount -gt 0) { [math]::Round($profitableCount / $ranCount, 4) } else { 0.0 }
$oosProfitSum = if ($ranCount -gt 0) { [math]::Round((($ran | Measure-Object -Property test_profit -Sum).Sum), 4) } else { 0.0 }
$oosMaxMdd = if ($ranCount -gt 0) { [math]::Round((($ran | Measure-Object -Property test_mdd_pct -Maximum).Maximum), 4) } else { 0.0 }

# Conservative walk-forward readiness:
# - at least 3 active OOS windows
# - OOS profitable ratio >= 55%
# - OOS total profit > 0
# - OOS max MDD <= 12%
$isReady = (
    ($ranCount -ge 3) -and
    ($profitRatio -ge 0.55) -and
    ($oosProfitSum -gt 0.0) -and
    ($oosMaxMdd -le 12.0)
)

$report = [pscustomobject]@{
    generated_at_utc              = (Get-Date).ToUniversalTime().ToString("o")
    input_csv                     = (Resolve-Path $InputCsv).Path
    windows_total                 = $windows.Count
    windows_oos_ran               = $ranCount
    oos_profitable_windows        = $profitableCount
    oos_profitable_ratio          = $profitRatio
    oos_total_profit              = $oosProfitSum
    oos_max_mdd_pct               = $oosMaxMdd
    gate_oos_windows_min          = 3
    gate_oos_profitable_ratio_min = 0.55
    gate_oos_profit_sum_positive  = $true
    gate_oos_max_mdd_pct_max      = 12.0
    is_ready_for_live_walkforward = $isReady
}

$report | ConvertTo-Json -Depth 4 | Set-Content -Path $OutputJson -Encoding UTF8

Write-Output "=== Walk-Forward Windows ==="
$windows | Format-Table -AutoSize | Out-String -Width 240 | Write-Output
Write-Output "=== Walk-Forward Verdict ==="
$report | Format-List | Out-String | Write-Output
Write-Output "saved_csv=$OutputCsv"
Write-Output "saved_json=$OutputJson"
