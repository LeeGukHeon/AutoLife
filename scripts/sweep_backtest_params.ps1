param(
    [string]$ConfigPath = ".\config\config.json",
    [double[]]$RewardRiskValues = @(1.2, 1.5, 1.8, 2.2, 2.8),
    [double[]]$ExpectedEdgeValues = @(0.0012, 0.0020, 0.0030, 0.0050, 0.0080),
    [string]$ValidateScript = ".\scripts\validate_small_seed.ps1",
    [string]$OutCsv = ".\build\Release\logs\param_sweep_results.csv"
)

$ErrorActionPreference = "Stop"

function Resolve-OrFail([string]$path, [string]$label) {
    $resolved = Resolve-Path $path -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "$label not found: $path"
    }
    return $resolved.Path
}

function Clamp([double]$v, [double]$min, [double]$max) {
    if ($v -lt $min) { return $min }
    if ($v -gt $max) { return $max }
    return $v
}

$configAbs = Resolve-OrFail $ConfigPath "config"
$validateAbs = Resolve-OrFail $ValidateScript "validate script"
$backup = "$configAbs.bak_sweep"

Copy-Item $configAbs $backup -Force

$rows = New-Object System.Collections.Generic.List[object]

try {
    foreach ($rr in $RewardRiskValues) {
        foreach ($edge in $ExpectedEdgeValues) {
            $cfg = Get-Content $configAbs -Raw | ConvertFrom-Json
            $cfg.trading.min_reward_risk = [double]$rr
            $cfg.trading.min_expected_edge_pct = [double]$edge
            ($cfg | ConvertTo-Json -Depth 20) | Set-Content -Path $configAbs -Encoding UTF8

            powershell -ExecutionPolicy Bypass -File $validateAbs | Out-Null
            $summary = Get-Content .\build\Release\logs\small_seed_summary.json -Raw | ConvertFrom-Json

            $seed50 = $summary.results | Where-Object { [double]$_.seed_krw -eq 50000 }
            $seed100 = $summary.results | Where-Object { [double]$_.seed_krw -eq 100000 }

            $s50Profit = if ($seed50) { [double]$seed50.profitable_ratio } else { 0.0 }
            $s50Tradable = if ($seed50) { [double]$seed50.tradable_ratio } else { 0.0 }
            $s50AvgProfit = if ($seed50) { [double]$seed50.avg_profit } else { 0.0 }
            $s100Profit = if ($seed100) { [double]$seed100.profitable_ratio } else { 0.0 }
            $s100Tradable = if ($seed100) { [double]$seed100.tradable_ratio } else { 0.0 }
            $s100AvgProfit = if ($seed100) { [double]$seed100.avg_profit } else { 0.0 }

            # Higher is better. Focus 100k edge recovery while keeping 50k alive.
            $score =
                ($s100Profit * 2.0) +
                ($s100Tradable * 0.8) +
                ($s50Profit * 0.6) +
                ($s50Tradable * 0.3) +
                (Clamp ($s100AvgProfit / 1000.0) -1.0 1.0) +
                (Clamp ($s50AvgProfit / 500.0) -0.5 0.5)

            $rows.Add([pscustomobject]@{
                min_reward_risk = [double]$rr
                min_expected_edge_pct = [double]$edge
                score = [double]$score
                seed50_profitable_ratio = $s50Profit
                seed50_tradable_ratio = $s50Tradable
                seed50_avg_profit = $s50AvgProfit
                seed100_profitable_ratio = $s100Profit
                seed100_tradable_ratio = $s100Tradable
                seed100_avg_profit = $s100AvgProfit
            }) | Out-Null

            Write-Host ("rr={0} edge={1} => score={2:N3} | 100k avg={3:N1}" -f $rr, $edge, $score, $s100AvgProfit)
        }
    }
}
finally {
    Copy-Item $backup $configAbs -Force
    Remove-Item $backup -Force -ErrorAction SilentlyContinue
}

$outDir = Split-Path -Parent $OutCsv
if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
$rows | Sort-Object -Property score -Descending | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8

Write-Host "Sweep complete: $OutCsv"
