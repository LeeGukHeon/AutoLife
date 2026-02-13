param(
    [string]$ExePath = ".\build\Release\AutoLifeTrading.exe",
    [string]$DataDir = ".\data\backtest_curated",
    [string]$OutputCsv = ".\build\Release\logs\small_seed_matrix.csv",
    [string]$OutputJson = ".\build\Release\logs\small_seed_summary.json",
    [double[]]$Seeds = @(50000, 100000),
    [int]$MinTrades = 5
)

$ErrorActionPreference = "Stop"

function Resolve-OrFail([string]$path, [string]$label) {
    $resolved = Resolve-Path $path -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "$label not found: $path"
    }
    return $resolved.Path
}

$exe = Resolve-OrFail $ExePath "exe"
$dataRoot = Resolve-OrFail $DataDir "data dir"

$csvDir = Split-Path -Parent $OutputCsv
$jsonDir = Split-Path -Parent $OutputJson
if ($csvDir) { New-Item -ItemType Directory -Force -Path $csvDir | Out-Null }
if ($jsonDir) { New-Item -ItemType Directory -Force -Path $jsonDir | Out-Null }

$datasets = Get-ChildItem -Path $dataRoot -File -Filter *.csv | Sort-Object Name
if (-not $datasets -or $datasets.Count -eq 0) {
    throw "No CSV datasets found in $dataRoot"
}

$rows = New-Object System.Collections.Generic.List[object]

foreach ($seed in $Seeds) {
    foreach ($ds in $datasets) {
        $args = @("--backtest", $ds.FullName, "--json", "--initial-capital", $seed.ToString())
        $raw = & $exe @args 2>&1

        $jsonLine = $null
        for ($i = $raw.Count - 1; $i -ge 0; $i--) {
            $line = [string]$raw[$i]
            if ($line.TrimStart().StartsWith("{")) {
                $jsonLine = $line
                break
            }
        }
        if (-not $jsonLine) {
            throw "No JSON result found for dataset=$($ds.Name), seed=$seed"
        }

        $r = $jsonLine | ConvertFrom-Json
        $isProfitable = [double]$r.total_profit -gt 0
        $isTradable = [int]$r.total_trades -ge $MinTrades
        $rows.Add([pscustomobject]@{
            seed_krw       = [double]$seed
            dataset        = $ds.Name
            final_balance  = [double]$r.final_balance
            total_profit   = [double]$r.total_profit
            total_trades   = [int]$r.total_trades
            win_rate       = [double]$r.win_rate
            profit_factor  = [double]$r.profit_factor
            max_drawdown   = [double]$r.max_drawdown
            is_profitable  = $isProfitable
            is_tradable    = $isTradable
        }) | Out-Null
    }
}

$rows | Sort-Object seed_krw, dataset | Export-Csv -Path $OutputCsv -NoTypeInformation -Encoding UTF8

$summaryBySeed = @()
foreach ($seed in $Seeds) {
    $group = $rows | Where-Object { $_.seed_krw -eq [double]$seed }
    if (-not $group -or $group.Count -eq 0) { continue }

    $profitable = ($group | Where-Object { $_.is_profitable.ToString().ToLowerInvariant() -eq "true" }).Count
    $tradable = ($group | Where-Object { $_.is_tradable.ToString().ToLowerInvariant() -eq "true" }).Count
    $summaryBySeed += [pscustomobject]@{
        seed_krw = [double]$seed
        datasets = [int]$group.Count
        profitable_datasets = [int]$profitable
        tradable_datasets = [int]$tradable
        profitable_ratio = if ($group.Count -gt 0) { [double]$profitable / [double]$group.Count } else { 0.0 }
        tradable_ratio = if ($group.Count -gt 0) { [double]$tradable / [double]$group.Count } else { 0.0 }
        avg_profit = ($group | Measure-Object -Property total_profit -Average).Average
        avg_drawdown = ($group | Measure-Object -Property max_drawdown -Average).Average
    }
}

$summary = [pscustomobject]@{
    generated_at = (Get-Date).ToString("s")
    exe_path = $exe
    data_dir = $dataRoot
    min_trades = $MinTrades
    seeds = $Seeds
    results = $summaryBySeed
}
$summary | ConvertTo-Json -Depth 6 | Out-File -FilePath $OutputJson -Encoding UTF8

Write-Host "Small-seed validation complete."
Write-Host "CSV: $OutputCsv"
Write-Host "JSON: $OutputJson"
