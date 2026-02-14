param(
    [string]$FetchScript = ".\scripts\fetch_upbit_historical_candles.ps1",
    [string]$MatrixScript = ".\scripts\run_profitability_matrix.ps1",
    [string]$TuneScript = ".\scripts\tune_candidate_gate_trade_density.ps1",
    [string]$RealDataDir = ".\data\backtest_real",
    [string]$BacktestDataDir = ".\data\backtest",
    [string]$CuratedDataDir = ".\data\backtest_curated",
    [string[]]$Markets = @(
        "KRW-BTC",
        "KRW-ETH",
        "KRW-XRP",
        "KRW-SOL",
        "KRW-DOGE",
        "KRW-ADA",
        "KRW-AVAX",
        "KRW-LINK",
        "KRW-DOT",
        "KRW-TRX",
        "KRW-SUI",
        "KRW-BCH"
    ),
    [string]$Unit = "1",
    [int]$Candles = 12000,
    [int]$Candles5m = 4000,
    [int]$Candles1h = 1200,
    [int]$Candles4h = 600,
    [int]$ChunkSize = 200,
    [int]$SleepMs = 120,
    [string]$OutputMatrixCsv = ".\build\Release\logs\profitability_matrix_realdata.csv",
    [string]$OutputProfileCsv = ".\build\Release\logs\profitability_profile_summary_realdata.csv",
    [string]$OutputReportJson = ".\build\Release\logs\profitability_gate_report_realdata.json",
    [switch]$SkipFetch,
    [switch]$SkipHigherTfFetch,
    [switch]$SkipTune,
    [switch]$RealDataOnly,
    [switch]$RequireHigherTfCompanions
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-OrThrow {
    param(
        [string]$PathValue,
        [string]$Label
    )
    $resolved = Resolve-Path -Path $PathValue -ErrorAction SilentlyContinue
    if ($null -eq $resolved) {
        throw "$Label not found: $PathValue"
    }
    return $resolved.Path
}

function Ensure-Directory {
    param([string]$PathValue)
    if (-not (Test-Path $PathValue)) {
        New-Item -ItemType Directory -Path $PathValue -Force | Out-Null
    }
}

function Build-RealDataFilePath {
    param(
        [string]$OutputDir,
        [string]$Market,
        [string]$TfUnit,
        [int]$RowCount
    )
    $safeMarket = $Market.Replace("-", "_")
    return Join-Path $OutputDir ("upbit_{0}_{1}m_{2}.csv" -f $safeMarket, $TfUnit, $RowCount)
}

function Test-HasHigherTfCompanions {
    param([string]$PrimaryPath)

    if ([string]::IsNullOrWhiteSpace($PrimaryPath) -or -not (Test-Path $PrimaryPath)) {
        return $false
    }

    $stem = [System.IO.Path]::GetFileNameWithoutExtension($PrimaryPath).ToLowerInvariant()
    if (-not $stem.StartsWith("upbit_") -or -not $stem.Contains("_1m_")) {
        return $false
    }

    $pivot = $stem.IndexOf("_1m_")
    if ($pivot -le 6) {
        return $false
    }

    $marketToken = $stem.Substring(6, $pivot - 6)
    $dirPath = Split-Path -Parent $PrimaryPath
    foreach ($tf in @("5m", "60m", "240m")) {
        $pattern = ("upbit_{0}_{1}_*.csv" -f $marketToken, $tf)
        $hits = @(Get-ChildItem -Path $dirPath -File -Filter $pattern -ErrorAction SilentlyContinue)
        if ($hits.Count -eq 0) {
            return $false
        }
    }

    return $true
}

function Get-DatasetFiles {
    param(
        [string[]]$Dirs,
        [switch]$OnlyRealData,
        [switch]$RequireHigherTfCompanions
    )
    $items = @()
    foreach ($dirPath in $Dirs) {
        if (-not (Test-Path $dirPath)) {
            continue
        }
        $isRealDataDir = $dirPath.ToLowerInvariant().Contains("backtest_real")
        if ($OnlyRealData.IsPresent -and -not $isRealDataDir) {
            continue
        }

        $candidates = @(
            Get-ChildItem -Path $dirPath -File -Filter *.csv -ErrorAction SilentlyContinue |
                Sort-Object Name |
                Where-Object {
                    if ($isRealDataDir) {
                        return $_.Name.ToLowerInvariant().Contains("_1m_")
                    }
                    return (-not $OnlyRealData.IsPresent)
                }
        )

        foreach ($file in $candidates) {
            if ($RequireHigherTfCompanions.IsPresent -and
                $isRealDataDir -and
                -not (Test-HasHigherTfCompanions -PrimaryPath $file.FullName)) {
                continue
            }
            $items += $file.FullName
        }
    }
    return @($items | Sort-Object -Unique)
}

$resolvedFetchScript = Resolve-OrThrow -PathValue $FetchScript -Label "Fetch script"
$resolvedMatrixScript = Resolve-OrThrow -PathValue $MatrixScript -Label "Matrix script"
$resolvedTuneScript = Resolve-OrThrow -PathValue $TuneScript -Label "Tune script"

$resolvedRealDataDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $RealDataDir))
$resolvedBacktestDataDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $BacktestDataDir))
$resolvedCuratedDataDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $CuratedDataDir))
Ensure-Directory -PathValue $resolvedRealDataDir

if (-not $SkipFetch.IsPresent) {
    foreach ($market in $Markets) {
        if ([string]::IsNullOrWhiteSpace($market)) {
            continue
        }

        $targetPath = Build-RealDataFilePath -OutputDir $resolvedRealDataDir -Market $market -TfUnit $Unit -RowCount $Candles
        Write-Host ("[RealDataLoop] Fetching market={0}, unit={1}m, candles={2}" -f $market, $Unit, $Candles)

        & $resolvedFetchScript `
            -Market $market `
            -Unit $Unit `
            -Candles $Candles `
            -ChunkSize $ChunkSize `
            -SleepMs $SleepMs `
            -OutputPath $targetPath | Out-Null

        if (-not $SkipHigherTfFetch.IsPresent) {
            $target5m = Build-RealDataFilePath -OutputDir $resolvedRealDataDir -Market $market -TfUnit "5" -RowCount $Candles5m
            Write-Host ("[RealDataLoop] Fetching market={0}, unit=5m, candles={1}" -f $market, $Candles5m)
            & $resolvedFetchScript `
                -Market $market `
                -Unit "5" `
                -Candles $Candles5m `
                -ChunkSize $ChunkSize `
                -SleepMs $SleepMs `
                -OutputPath $target5m | Out-Null

            $target1h = Build-RealDataFilePath -OutputDir $resolvedRealDataDir -Market $market -TfUnit "60" -RowCount $Candles1h
            Write-Host ("[RealDataLoop] Fetching market={0}, unit=60m, candles={1}" -f $market, $Candles1h)
            & $resolvedFetchScript `
                -Market $market `
                -Unit "60" `
                -Candles $Candles1h `
                -ChunkSize $ChunkSize `
                -SleepMs $SleepMs `
                -OutputPath $target1h | Out-Null

            $target4h = Build-RealDataFilePath -OutputDir $resolvedRealDataDir -Market $market -TfUnit "240" -RowCount $Candles4h
            Write-Host ("[RealDataLoop] Fetching market={0}, unit=240m, candles={1}" -f $market, $Candles4h)
            & $resolvedFetchScript `
                -Market $market `
                -Unit "240" `
                -Candles $Candles4h `
                -ChunkSize $ChunkSize `
                -SleepMs $SleepMs `
                -OutputPath $target4h | Out-Null
        }
    }
}

$datasetDirs = if ($RealDataOnly.IsPresent) {
    @($resolvedRealDataDir)
} else {
    @($resolvedBacktestDataDir, $resolvedCuratedDataDir, $resolvedRealDataDir)
}
$datasets = Get-DatasetFiles `
    -Dirs $datasetDirs `
    -OnlyRealData:$RealDataOnly `
    -RequireHigherTfCompanions:$RequireHigherTfCompanions
if (@($datasets).Count -eq 0) {
    throw "No datasets found after fetch step."
}
Write-Host ("[RealDataLoop] dataset_mode={0}, require_higher_tf={1}, dataset_count={2}" -f `
    $(if ($RealDataOnly.IsPresent) { "realdata_only" } else { "mixed" }), `
    $RequireHigherTfCompanions.IsPresent, `
    @($datasets).Count)

Write-Host ("[RealDataLoop] Running profitability matrix with datasets={0}" -f @($datasets).Count)
& $resolvedMatrixScript `
    -DatasetNames $datasets `
    -ExcludeLowTradeRunsForGate `
    -MinTradesPerRunForGate 1 `
    -OutputCsv $OutputMatrixCsv `
    -OutputProfileCsv $OutputProfileCsv `
    -OutputJson $OutputReportJson | Out-Null

$resolvedOutputReport = Resolve-OrThrow -PathValue $OutputReportJson -Label "Gate report"
$report = Get-Content -Raw -Path $resolvedOutputReport -Encoding UTF8 | ConvertFrom-Json
$coreFull = $report.profile_summaries | Where-Object { $_.profile_id -eq "core_full" } | Select-Object -First 1

if ($null -ne $coreFull) {
    Write-Host ("[RealDataLoop] core_full.avg_profit_factor={0}" -f $coreFull.avg_profit_factor)
    Write-Host ("[RealDataLoop] core_full.avg_total_trades={0}" -f $coreFull.avg_total_trades)
    Write-Host ("[RealDataLoop] core_full.avg_expectancy_krw={0}" -f $coreFull.avg_expectancy_krw)
}
Write-Host ("[RealDataLoop] overall_gate_pass={0}" -f $report.overall_gate_pass)

if (-not $SkipTune.IsPresent) {
    Write-Host "[RealDataLoop] Running candidate trade-density tuning with real datasets included"
    $tuneArgs = @(
        "-DataDir", $resolvedBacktestDataDir,
        "-CuratedDataDir", $resolvedCuratedDataDir,
        "-ExtraDataDirs", $resolvedRealDataDir
    )
    if ($RealDataOnly.IsPresent) {
        $tuneArgs += "-RealDataOnly"
    }
    if ($RequireHigherTfCompanions.IsPresent) {
        $tuneArgs += "-RequireHigherTfCompanions"
    }
    & $resolvedTuneScript @tuneArgs | Out-Null
}

Write-Host "[RealDataLoop] Completed"
Write-Host ("gate_report={0}" -f $resolvedOutputReport)
