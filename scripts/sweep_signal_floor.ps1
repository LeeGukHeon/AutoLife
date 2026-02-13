param(
    [string]$ConfigPath = ".\config\config.json",
    [double[]]$SignalFloors = @(0.45, 0.55, 0.65, 0.75),
    [string]$ValidateScript = ".\scripts\validate_small_seed.ps1",
    [string]$OutCsv = ".\build\Release\logs\signal_floor_sweep.csv"
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
$backup = "$configAbs.bak_floor"
Copy-Item $configAbs $backup -Force

$rows = New-Object System.Collections.Generic.List[object]

try {
    foreach ($floor in $SignalFloors) {
        $cfg = Get-Content $configAbs -Raw | ConvertFrom-Json
        if (-not $cfg.strategies) { $cfg | Add-Member -NotePropertyName strategies -NotePropertyValue ([pscustomobject]@{}) }
        foreach ($name in @("scalping","momentum","breakout","mean_reversion")) {
            if (-not $cfg.strategies.$name) {
                $cfg.strategies | Add-Member -NotePropertyName $name -NotePropertyValue ([pscustomobject]@{})
            }
            if ($null -eq ($cfg.strategies.$name.PSObject.Properties["min_signal_strength"])) {
                $cfg.strategies.$name | Add-Member -NotePropertyName min_signal_strength -NotePropertyValue ([double]$floor)
            } else {
                $cfg.strategies.$name.min_signal_strength = [double]$floor
            }
        }
        ($cfg | ConvertTo-Json -Depth 20) | Set-Content -Path $configAbs -Encoding UTF8

        & $validateAbs | Out-Null
        $summary = Get-Content .\build\Release\logs\small_seed_summary.json -Raw | ConvertFrom-Json

        $seed50 = $summary.results | Where-Object { [double]$_.seed_krw -eq 50000 }
        $seed100 = $summary.results | Where-Object { [double]$_.seed_krw -eq 100000 }

        $s50Profit = if ($seed50) { [double]$seed50.profitable_ratio } else { 0.0 }
        $s50Tradable = if ($seed50) { [double]$seed50.tradable_ratio } else { 0.0 }
        $s50AvgProfit = if ($seed50) { [double]$seed50.avg_profit } else { 0.0 }
        $s100Profit = if ($seed100) { [double]$seed100.profitable_ratio } else { 0.0 }
        $s100Tradable = if ($seed100) { [double]$seed100.tradable_ratio } else { 0.0 }
        $s100AvgProfit = if ($seed100) { [double]$seed100.avg_profit } else { 0.0 }

        $score =
            ($s100Profit * 2.0) +
            ($s100Tradable * 0.8) +
            ($s50Profit * 0.6) +
            ($s50Tradable * 0.3) +
            (Clamp ($s100AvgProfit / 1000.0) -1.0 1.0) +
            (Clamp ($s50AvgProfit / 500.0) -0.5 0.5)

        $rows.Add([pscustomobject]@{
            signal_floor = [double]$floor
            score = [double]$score
            seed50_profitable_ratio = $s50Profit
            seed50_tradable_ratio = $s50Tradable
            seed50_avg_profit = $s50AvgProfit
            seed100_profitable_ratio = $s100Profit
            seed100_tradable_ratio = $s100Tradable
            seed100_avg_profit = $s100AvgProfit
        }) | Out-Null

        Write-Host ("floor={0} => score={1:N3} | 100k avg={2:N1}" -f $floor, $score, $s100AvgProfit)
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
