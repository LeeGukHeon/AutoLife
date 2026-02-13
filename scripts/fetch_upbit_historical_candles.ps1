param(
    [string]$Market = "KRW-BTC",
    [ValidateSet("1","3","5","10","15","30","60","240")]
    [string]$Unit = "1",
    [int]$Candles = 12000,
    [string]$OutputPath = "",
    [string]$EndUtc = "",
    [int]$ChunkSize = 200,
    [int]$SleepMs = 120,
    [string]$BaseUrl = "https://api.upbit.com"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Candles -le 0) {
    throw "Candles must be > 0."
}
if ($ChunkSize -le 0 -or $ChunkSize -gt 200) {
    throw "ChunkSize must be between 1 and 200."
}
if ($SleepMs -lt 0) {
    throw "SleepMs must be >= 0."
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $safeMarket = $Market.Replace("-", "_")
    $OutputPath = ".\data\backtest_real\upbit_{0}_{1}m_{2}.csv" -f $safeMarket, $Unit, $Candles
}

$cursorUtc = $null
if (-not [string]::IsNullOrWhiteSpace($EndUtc)) {
    $cursorUtc = [DateTimeOffset]::Parse($EndUtc).ToUniversalTime()
}

function Build-QueryString {
    param([hashtable]$Params)
    $pairs = @()
    foreach ($key in $Params.Keys) {
        $k = [System.Uri]::EscapeDataString([string]$key)
        $v = [System.Uri]::EscapeDataString([string]$Params[$key])
        $pairs += ("{0}={1}" -f $k, $v)
    }
    return ($pairs -join "&")
}

function To-UnixMs {
    param($Item)
    if ($Item.PSObject.Properties["timestamp"]) {
        return [int64]$Item.timestamp
    }
    if ($Item.PSObject.Properties["candle_date_time_utc"]) {
        return [DateTimeOffset]::Parse($Item.candle_date_time_utc).ToUnixTimeMilliseconds()
    }
    throw "Cannot derive timestamp from candle payload."
}

$endpoint = "/v1/candles/minutes/$Unit"
$allRows = New-Object "System.Collections.Generic.List[object]"

while ($allRows.Count -lt $Candles) {
    $remaining = $Candles - $allRows.Count
    $count = [Math]::Min($ChunkSize, $remaining)

    $query = @{
        market = $Market
        count = $count
    }
    if ($null -ne $cursorUtc) {
        $query["to"] = $cursorUtc.ToString("yyyy-MM-ddTHH:mm:ssZ")
    }

    $url = "{0}{1}?{2}" -f $BaseUrl.TrimEnd("/"), $endpoint, (Build-QueryString -Params $query)
    $resp = Invoke-RestMethod -Method Get -Uri $url -TimeoutSec 30
    $batch = @($resp)

    if ($batch.Count -eq 0) {
        break
    }

    foreach ($item in $batch) {
        $ts = To-UnixMs -Item $item
        $allRows.Add([pscustomobject]@{
            timestamp = [int64]$ts
            open = [double]$item.opening_price
            high = [double]$item.high_price
            low = [double]$item.low_price
            close = [double]$item.trade_price
            volume = [double]$item.candle_acc_trade_volume
        }) | Out-Null
    }

    $oldest = $batch | ForEach-Object { To-UnixMs -Item $_ } | Measure-Object -Minimum
    $cursorUtc = [DateTimeOffset]::FromUnixTimeMilliseconds([int64]$oldest.Minimum).AddMilliseconds(-1).ToUniversalTime()

    if ($SleepMs -gt 0) {
        Start-Sleep -Milliseconds $SleepMs
    }
}

$rows = @(
    $allRows |
    Sort-Object timestamp |
    Group-Object timestamp |
    ForEach-Object { $_.Group[0] }
)

if ($rows.Count -gt $Candles) {
    $rows = @($rows | Select-Object -Last $Candles)
}

$parent = Split-Path -Parent $OutputPath
if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path $parent)) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}

$inv = [System.Globalization.CultureInfo]::InvariantCulture
$lines = New-Object "System.Collections.Generic.List[string]"
$lines.Add("timestamp,open,high,low,close,volume")
foreach ($r in $rows) {
    $line = "{0},{1},{2},{3},{4},{5}" -f `
        ([int64]$r.timestamp), `
        ([double]$r.open).ToString($inv), `
        ([double]$r.high).ToString($inv), `
        ([double]$r.low).ToString($inv), `
        ([double]$r.close).ToString($inv), `
        ([double]$r.volume).ToString($inv)
    $lines.Add($line)
}
Set-Content -Path $OutputPath -Value $lines -Encoding UTF8

if ($rows.Count -eq 0) {
    throw "No candles fetched. Check market/unit/time range."
}

$firstTs = [int64]$rows[0].timestamp
$lastTs = [int64]$rows[$rows.Count - 1].timestamp
$firstUtc = [DateTimeOffset]::FromUnixTimeMilliseconds($firstTs).ToString("o")
$lastUtc = [DateTimeOffset]::FromUnixTimeMilliseconds($lastTs).ToString("o")

Write-Host "[FetchUpbitCandles] Completed"
Write-Host ("market={0}" -f $Market)
Write-Host ("unit={0}m" -f $Unit)
Write-Host ("rows={0}" -f $rows.Count)
Write-Host ("from_utc={0}" -f $firstUtc)
Write-Host ("to_utc={0}" -f $lastUtc)
Write-Host ("output={0}" -f (Resolve-Path $OutputPath).Path)
