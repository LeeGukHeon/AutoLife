param(
    [string]$DataDir = ".\\data\\backtest",
    [string]$OutputCsv = ".\\build\\Release\\logs\\backtest_data_audit.csv",
    [string]$OutputJson = ".\\build\\Release\\logs\\backtest_data_audit.json"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $DataDir)) {
    throw "Backtest data dir not found: $DataDir"
}

$files = Get-ChildItem -Path $DataDir -File -Filter *.csv | Sort-Object FullName
if ($files.Count -eq 0) {
    throw "No CSV files found in $DataDir"
}

$rows = @()
foreach ($f in $files) {
    $c = Import-Csv $f.FullName
    if ($c.Count -lt 2) {
        continue
    }

    $ts = @($c | ForEach-Object { [double]$_.timestamp })
    $open = @($c | ForEach-Object { [double]$_.open })
    $high = @($c | ForEach-Object { [double]$_.high })
    $low = @($c | ForEach-Object { [double]$_.low })
    $close = @($c | ForEach-Object { [double]$_.close })
    $vol = @($c | ForEach-Object { [double]$_.volume })

    $dts = @()
    for ($i = 1; $i -lt $ts.Count; $i++) {
        $dts += ($ts[$i] - $ts[$i - 1])
    }
    $modeDt = ($dts | Group-Object | Sort-Object Count -Descending | Select-Object -First 1).Name

    $returns = @()
    for ($i = 1; $i -lt $close.Count; $i++) {
        if ($close[$i - 1] -ne 0) {
            $returns += (($close[$i] - $close[$i - 1]) / $close[$i - 1])
        }
    }
    $avgAbsRet = if ($returns.Count -gt 0) {
        ($returns | ForEach-Object { [math]::Abs($_) } | Measure-Object -Average).Average
    } else { 0.0 }
    $maxAbsRet = if ($returns.Count -gt 0) {
        ($returns | ForEach-Object { [math]::Abs($_) } | Measure-Object -Maximum).Maximum
    } else { 0.0 }

    $badOhlc = 0
    for ($i = 0; $i -lt $c.Count; $i++) {
        if ($high[$i] -lt $open[$i] -or $high[$i] -lt $close[$i] -or
            $low[$i] -gt $open[$i] -or $low[$i] -gt $close[$i] -or
            $high[$i] -lt $low[$i]) {
            $badOhlc++
        }
    }

    $tsUnit = if ($ts[0] -gt 1e12) { "ms" } else { "sec" }
    $tooShort = ($c.Count -lt 1000)
    $lowRegimeCoverage = ($c.Count -lt 300)
    $timeJitter = (($dts | Select-Object -Unique).Count -gt 1)
    $veryLowShock = (($maxAbsRet * 100.0) -lt 1.0)

    $rows += [pscustomobject]@{
        file               = $f.Name
        rows               = $c.Count
        ts_unit            = $tsUnit
        mode_dt            = [double]$modeDt
        dt_unique          = ($dts | Select-Object -Unique).Count
        bad_ohlc_rows      = $badOhlc
        avg_abs_ret_pct    = [math]::Round($avgAbsRet * 100.0, 4)
        max_abs_ret_pct    = [math]::Round($maxAbsRet * 100.0, 4)
        avg_volume         = [math]::Round(($vol | Measure-Object -Average).Average, 4)
        too_short_for_tune = $tooShort
        low_regime_coverage = $lowRegimeCoverage
        timestamp_mixed_risk = $false
        time_jitter_risk   = $timeJitter
        low_shock_risk     = $veryLowShock
    }
}

$hasSec = @($rows | Where-Object { $_.ts_unit -eq "sec" }).Count -gt 0
$hasMs = @($rows | Where-Object { $_.ts_unit -eq "ms" }).Count -gt 0
if ($hasSec -and $hasMs) {
    foreach ($r in $rows) {
        $r.timestamp_mixed_risk = $true
    }
}

$rows | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputCsv

$summary = [pscustomobject]@{
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    dataset_count = $rows.Count
    has_mixed_timestamp_units = ($hasSec -and $hasMs)
    short_datasets = @($rows | Where-Object { $_.too_short_for_tune }).Count
    low_regime_coverage_datasets = @($rows | Where-Object { $_.low_regime_coverage }).Count
    low_shock_datasets = @($rows | Where-Object { $_.low_shock_risk }).Count
    bad_ohlc_datasets = @($rows | Where-Object { $_.bad_ohlc_rows -gt 0 }).Count
}

$report = [pscustomobject]@{
    summary = $summary
    files = $rows
}

$report | ConvertTo-Json -Depth 5 | Set-Content -Path $OutputJson -Encoding UTF8

Write-Output "=== Backtest Data Audit ==="
$rows | Format-Table -AutoSize | Out-String -Width 240 | Write-Output
Write-Output "=== Audit Summary ==="
$summary | Format-List | Out-String | Write-Output
Write-Output "saved_csv=$OutputCsv"
Write-Output "saved_json=$OutputJson"
