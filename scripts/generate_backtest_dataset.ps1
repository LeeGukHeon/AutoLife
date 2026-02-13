param(
    [string]$OutputPath = ".\\data\\backtest_curated\\regime_mix_12000_1m.csv",
    [int]$Candles = 12000,
    [double]$StartPrice = 1000000.0,
    [int]$Seed = 20260213,
    [ValidateSet("mixed","range_bias","trend_down_shock")]
    [string]$Profile = "mixed"
)

$ErrorActionPreference = "Stop"

if ($Candles -lt 1000) {
    throw "Candles must be >= 1000 for regime-mix test data."
}
if ($StartPrice -le 0) {
    throw "StartPrice must be > 0."
}

$dir = Split-Path -Parent $OutputPath
if (![string]::IsNullOrWhiteSpace($dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$rand = [System.Random]::new($Seed)
$nowMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$ts = $nowMs - ([int64]$Candles * 60000)
$price = $StartPrice

$regimes = @(
    [pscustomobject]@{ name = "TRENDING_UP"; drift = 0.00035; sigma = 0.0018; volMul = 1.2; weight = 1.0 },
    [pscustomobject]@{ name = "TRENDING_DOWN"; drift = -0.00035; sigma = 0.0020; volMul = 1.25; weight = 1.0 },
    [pscustomobject]@{ name = "RANGING"; drift = 0.00000; sigma = 0.0012; volMul = 0.95; weight = 1.0 },
    [pscustomobject]@{ name = "HIGH_VOL"; drift = 0.00000; sigma = 0.0038; volMul = 1.8; weight = 1.0 }
)

switch ($Profile) {
    "range_bias" {
        $regimes = @(
            [pscustomobject]@{ name = "RANGING"; drift = 0.00000; sigma = 0.0010; volMul = 0.90; weight = 4.0 },
            [pscustomobject]@{ name = "MEANREV_NOISE"; drift = 0.00000; sigma = 0.0014; volMul = 1.00; weight = 2.0 },
            [pscustomobject]@{ name = "TRENDING_UP"; drift = 0.00020; sigma = 0.0015; volMul = 1.05; weight = 1.0 },
            [pscustomobject]@{ name = "TRENDING_DOWN"; drift = -0.00020; sigma = 0.0016; volMul = 1.05; weight = 1.0 }
        )
    }
    "trend_down_shock" {
        $regimes = @(
            [pscustomobject]@{ name = "TRENDING_DOWN"; drift = -0.00045; sigma = 0.0024; volMul = 1.35; weight = 4.0 },
            [pscustomobject]@{ name = "HIGH_VOL"; drift = -0.00010; sigma = 0.0045; volMul = 2.0; weight = 2.0 },
            [pscustomobject]@{ name = "RANGING"; drift = 0.00000; sigma = 0.0015; volMul = 1.0; weight = 1.0 },
            [pscustomobject]@{ name = "TRENDING_UP"; drift = 0.00020; sigma = 0.0018; volMul = 1.1; weight = 1.0 }
        )
    }
}

function Pick-Regime([System.Random]$r, $candidates) {
    $total = ($candidates | Measure-Object -Property weight -Sum).Sum
    $x = $r.NextDouble() * $total
    $acc = 0.0
    foreach ($item in $candidates) {
        $acc += [double]$item.weight
        if ($x -le $acc) { return $item }
    }
    return $candidates[0]
}

function Get-NormalSample([System.Random]$r, [double]$mu, [double]$sigma) {
    $u1 = [Math]::Max(1e-12, $r.NextDouble())
    $u2 = $r.NextDouble()
    $z = [Math]::Sqrt(-2.0 * [Math]::Log($u1)) * [Math]::Cos(2.0 * [Math]::PI * $u2)
    return $mu + ($sigma * $z)
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("timestamp,open,high,low,close,volume")

$i = 0
while ($i -lt $Candles) {
    $regime = Pick-Regime $rand $regimes
    $blockLen = $rand.Next(180, 720)  # 3h ~ 12h blocks
    if ($i + $blockLen -gt $Candles) {
        $blockLen = $Candles - $i
    }

    for ($j = 0; $j -lt $blockLen; $j++) {
        $open = $price
        $ret = Get-NormalSample $rand $regime.drift $regime.sigma

        # occasional regime shocks
        $shockProb = 0.008
        if ($Profile -eq "trend_down_shock") { $shockProb = 0.018 }
        elseif ($Profile -eq "range_bias") { $shockProb = 0.006 }
        if ($rand.NextDouble() -lt $shockProb) {
            $shock = Get-NormalSample $rand 0.0 ([Math]::Max(0.0025, $regime.sigma * 2.8))
            $ret += $shock
        }

        $close = $open * (1.0 + $ret)
        if ($close -le 0) {
            $close = [Math]::Max(1.0, $open * 0.98)
        }

        $absRet = [Math]::Abs($ret)
        $wickBase = [Math]::Max(0.0004, $regime.sigma * 0.9)
        $wickU = $open * ($wickBase + ($rand.NextDouble() * $wickBase))
        $wickD = $open * ($wickBase + ($rand.NextDouble() * $wickBase))
        $high = [Math]::Max($open, $close) + $wickU
        $low = [Math]::Min($open, $close) - $wickD
        if ($low -le 0) {
            $low = [Math]::Max(0.1, [Math]::Min($open, $close) * 0.995)
        }

        $baseVol = 30.0 + ($rand.NextDouble() * 40.0)
        $vol = $baseVol * $regime.volMul * (1.0 + ($absRet * 140.0))
        if ($rand.NextDouble() -lt 0.03) {
            $vol *= (1.3 + ($rand.NextDouble() * 1.7))
        }

        $line = "{0},{1},{2},{3},{4},{5}" -f `
            $ts, `
            ([Math]::Round($open, 6).ToString([System.Globalization.CultureInfo]::InvariantCulture)), `
            ([Math]::Round($high, 6).ToString([System.Globalization.CultureInfo]::InvariantCulture)), `
            ([Math]::Round($low, 6).ToString([System.Globalization.CultureInfo]::InvariantCulture)), `
            ([Math]::Round($close, 6).ToString([System.Globalization.CultureInfo]::InvariantCulture)), `
            ([Math]::Round($vol, 6).ToString([System.Globalization.CultureInfo]::InvariantCulture))
        $lines.Add($line)

        $price = $close
        $ts += 60000
        $i++
    }
}

Set-Content -Path $OutputPath -Value $lines -Encoding UTF8
Write-Output "generated_file=$OutputPath"
Write-Output "rows=$Candles"
Write-Output "timestamp_unit=ms"
Write-Output "profile=$Profile"
