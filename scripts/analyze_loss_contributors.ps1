param(
    [string]$ExePath = ".\build\Release\AutoLifeTrading.exe",
    [string]$ConfigPath = ".\build\Release\config\config.json",
    [string]$GateReportJson = ".\build\Release\logs\profitability_gate_report_realdata.json",
    [string[]]$DataDirs = @(
        ".\data\backtest",
        ".\data\backtest_curated",
        ".\data\backtest_real"
    ),
    [string]$OutputMarketCsv = ".\build\Release\logs\loss_contributor_by_market.csv",
    [string]$OutputStrategyCsv = ".\build\Release\logs\loss_contributor_by_strategy.csv"
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

function Ensure-ParentDirectory {
    param([string]$PathValue)
    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path $parent)) {
        New-Item -Path $parent -ItemType Directory -Force | Out-Null
    }
}

function Set-OrAddProperty {
    param(
        $ObjectValue,
        [string]$Name,
        $Value
    )
    if ($null -eq $ObjectValue) {
        return
    }
    $prop = $ObjectValue.PSObject.Properties[$Name]
    if ($null -eq $prop) {
        $ObjectValue | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    } else {
        $ObjectValue.$Name = $Value
    }
}

function Ensure-ConfigScaffold {
    param($ConfigObject)

    if ($null -eq $ConfigObject.trading) {
        $ConfigObject | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
    }

    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_plane_bridge" -Value $false
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_policy_plane" -Value $false
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_risk_plane" -Value $false
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_execution_plane" -Value $false
}

function Apply-CoreFullFlags {
    param($ConfigObject)
    Ensure-ConfigScaffold -ConfigObject $ConfigObject
    $ConfigObject.trading.enable_core_plane_bridge = $true
    $ConfigObject.trading.enable_core_policy_plane = $true
    $ConfigObject.trading.enable_core_risk_plane = $true
    $ConfigObject.trading.enable_core_execution_plane = $true
}

function Invoke-BacktestJson {
    param(
        [string]$ExeFile,
        [string]$DatasetPath
    )

    $raw = & $ExeFile --backtest $DatasetPath --json 2>&1
    $jsonLine = $null
    for ($i = $raw.Count - 1; $i -ge 0; $i--) {
        $line = [string]$raw[$i]
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith("{") -and $trimmed.EndsWith("}")) {
            $jsonLine = $trimmed
            break
        }
    }
    if ($null -eq $jsonLine) {
        return $null
    }
    try {
        return ($jsonLine | ConvertFrom-Json -ErrorAction Stop)
    } catch {
        return $null
    }
}

function Resolve-DatasetPath {
    param(
        [string]$DatasetFileName,
        [hashtable]$DatasetLookup
    )
    $key = $DatasetFileName.ToLowerInvariant()
    if ($DatasetLookup.ContainsKey($key)) {
        return [string]$DatasetLookup[$key]
    }
    return $null
}

function Get-MarketLabelFromDataset {
    param([string]$DatasetFileName)
    $name = [System.IO.Path]::GetFileNameWithoutExtension($DatasetFileName)
    $parts = $name.Split("_")
    if ($parts.Length -ge 3 -and $parts[0].ToLowerInvariant() -eq "upbit") {
        return ("{0}-{1}" -f $parts[1].ToUpperInvariant(), $parts[2].ToUpperInvariant())
    }
    return ("synthetic:{0}" -f $DatasetFileName)
}

$resolvedExePath = Resolve-OrThrow -PathValue $ExePath -Label "Executable"
$resolvedConfigPath = Resolve-OrThrow -PathValue $ConfigPath -Label "Runtime config"
$resolvedGateReport = Resolve-OrThrow -PathValue $GateReportJson -Label "Gate report"
$resolvedDataDirs = @()
foreach ($dirPath in $DataDirs) {
    $resolved = Resolve-Path -Path $dirPath -ErrorAction SilentlyContinue
    if ($null -ne $resolved) {
        $resolvedDataDirs += $resolved.Path
    }
}
if (@($resolvedDataDirs).Count -eq 0) {
    throw "No data directories were resolved."
}
$datasetLookup = @{}
foreach ($dirPath in $resolvedDataDirs) {
    $files = Get-ChildItem -Path $dirPath -File -Filter *.csv -ErrorAction SilentlyContinue
    foreach ($file in $files) {
        $key = $file.Name.ToLowerInvariant()
        if (-not $datasetLookup.ContainsKey($key)) {
            $datasetLookup[$key] = $file.FullName
        }
    }
}

$resolvedOutputMarketCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputMarketCsv))
$resolvedOutputStrategyCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputStrategyCsv))
Ensure-ParentDirectory -PathValue $resolvedOutputMarketCsv
Ensure-ParentDirectory -PathValue $resolvedOutputStrategyCsv

$report = Get-Content -Raw -Path $resolvedGateReport -Encoding UTF8 | ConvertFrom-Json
if ($null -eq $report.matrix_rows) {
    throw "Gate report does not contain matrix_rows: $resolvedGateReport"
}

$coreLossRows = @(
    $report.matrix_rows |
        Where-Object {
            $_.profile_id -eq "core_full" -and
            [double]$_.total_trades -gt 0 -and
            [double]$_.total_profit_krw -lt 0.0
        }
)

if (@($coreLossRows).Count -eq 0) {
    @() | Export-Csv -Path $resolvedOutputMarketCsv -NoTypeInformation -Encoding UTF8
    @() | Export-Csv -Path $resolvedOutputStrategyCsv -NoTypeInformation -Encoding UTF8
    Write-Host "[LossContrib] No negative core_full rows found. Empty outputs were written."
    Write-Host "market_csv=$resolvedOutputMarketCsv"
    Write-Host "strategy_csv=$resolvedOutputStrategyCsv"
    exit 0
}

$marketAgg = @{}
foreach ($row in $coreLossRows) {
    $datasetName = [string]$row.dataset
    $marketName = Get-MarketLabelFromDataset -DatasetFileName $datasetName
    $lossValue = [Math]::Abs([double]$row.total_profit_krw)
    $trades = [int]$row.total_trades

    if (-not $marketAgg.ContainsKey($marketName)) {
        $marketAgg[$marketName] = [ordered]@{
            market = $marketName
            loss_krw = 0.0
            total_trades = 0
            losing_runs = 0
        }
    }

    $marketAgg[$marketName].loss_krw += $lossValue
    $marketAgg[$marketName].total_trades += $trades
    $marketAgg[$marketName].losing_runs += 1
}

$totalMarketLoss = 0.0
foreach ($item in $marketAgg.Values) {
    $totalMarketLoss += [double]$item.loss_krw
}

$marketRows = @()
foreach ($item in $marketAgg.Values) {
    $loss = [double]$item.loss_krw
    $share = if ($totalMarketLoss -gt 0.0) { ($loss / $totalMarketLoss) * 100.0 } else { 0.0 }
    $marketRows += [pscustomobject]@{
        market = $item.market
        loss_krw = [Math]::Round($loss, 4)
        loss_share_pct = [Math]::Round($share, 4)
        total_trades = [int]$item.total_trades
        losing_runs = [int]$item.losing_runs
    }
}
$marketRows = @($marketRows | Sort-Object loss_krw -Descending)
$marketRows | Export-Csv -Path $resolvedOutputMarketCsv -NoTypeInformation -Encoding UTF8

$strategyAgg = @{}
$missingDatasets = New-Object System.Collections.Generic.List[string]
$backtestFailures = New-Object System.Collections.Generic.List[string]

$originalConfigRaw = Get-Content -Path $resolvedConfigPath -Encoding UTF8 | Out-String
try {
    $cfg = $originalConfigRaw | ConvertFrom-Json
    Apply-CoreFullFlags -ConfigObject $cfg
    ($cfg | ConvertTo-Json -Depth 32) | Set-Content -Path $resolvedConfigPath -Encoding UTF8

    foreach ($row in ($coreLossRows | Sort-Object {[double]$_.total_profit_krw})) {
        $datasetName = [string]$row.dataset
        $datasetPath = Resolve-DatasetPath -DatasetFileName $datasetName -DatasetLookup $datasetLookup
        if ([string]::IsNullOrWhiteSpace($datasetPath)) {
            $missingDatasets.Add($datasetName) | Out-Null
            continue
        }

        $result = Invoke-BacktestJson -ExeFile $resolvedExePath -DatasetPath $datasetPath
        if ($null -eq $result) {
            $backtestFailures.Add($datasetName) | Out-Null
            continue
        }
        if ($null -eq $result.strategy_summaries) {
            continue
        }

        foreach ($summary in $result.strategy_summaries) {
            $strategyName = [string]$summary.strategy_name
            if ([string]::IsNullOrWhiteSpace($strategyName)) {
                continue
            }
            $strategyProfit = [double]$summary.total_profit
            if ($strategyProfit -ge 0.0) {
                continue
            }
            $strategyLoss = [Math]::Abs($strategyProfit)
            $strategyTrades = [int]$summary.total_trades
            $strategyLosingTrades = [int]$summary.losing_trades

            if (-not $strategyAgg.ContainsKey($strategyName)) {
                $strategyAgg[$strategyName] = [ordered]@{
                    strategy_name = $strategyName
                    loss_krw = 0.0
                    total_trades = 0
                    losing_trades = 0
                    losing_datasets = 0
                }
            }

            $strategyAgg[$strategyName].loss_krw += $strategyLoss
            $strategyAgg[$strategyName].total_trades += $strategyTrades
            $strategyAgg[$strategyName].losing_trades += $strategyLosingTrades
            $strategyAgg[$strategyName].losing_datasets += 1
        }
    }
}
finally {
    $originalConfigRaw | Set-Content -Path $resolvedConfigPath -Encoding UTF8
}

$strategyTotalLoss = 0.0
foreach ($item in $strategyAgg.Values) {
    $strategyTotalLoss += [double]$item.loss_krw
}

$strategyRows = @()
foreach ($item in $strategyAgg.Values) {
    $loss = [double]$item.loss_krw
    $share = if ($strategyTotalLoss -gt 0.0) { ($loss / $strategyTotalLoss) * 100.0 } else { 0.0 }
    $strategyRows += [pscustomobject]@{
        strategy_name = $item.strategy_name
        loss_krw = [Math]::Round($loss, 4)
        loss_share_pct = [Math]::Round($share, 4)
        total_trades = [int]$item.total_trades
        losing_trades = [int]$item.losing_trades
        losing_datasets = [int]$item.losing_datasets
    }
}
$strategyRows = @($strategyRows | Sort-Object loss_krw -Descending)
$strategyRows | Export-Csv -Path $resolvedOutputStrategyCsv -NoTypeInformation -Encoding UTF8

Write-Host "[LossContrib] Completed"
Write-Host "market_csv=$resolvedOutputMarketCsv"
Write-Host "strategy_csv=$resolvedOutputStrategyCsv"
if (@($marketRows).Count -gt 0) {
    $topMarkets = @($marketRows | Select-Object -First 5)
    foreach ($m in $topMarkets) {
        Write-Host ("[LossContrib][Market] {0} | loss={1} | share={2}%" -f $m.market, $m.loss_krw, $m.loss_share_pct)
    }
}
if (@($strategyRows).Count -gt 0) {
    $topStrategies = @($strategyRows | Select-Object -First 2)
    foreach ($s in $topStrategies) {
        Write-Host ("[LossContrib][Strategy] {0} | loss={1} | share={2}%" -f $s.strategy_name, $s.loss_krw, $s.loss_share_pct)
    }
}
if ($missingDatasets.Count -gt 0) {
    Write-Host ("[LossContrib] missing_datasets={0}" -f (($missingDatasets | Sort-Object -Unique) -join ","))
}
if ($backtestFailures.Count -gt 0) {
    Write-Host ("[LossContrib] backtest_failures={0}" -f (($backtestFailures | Sort-Object -Unique) -join ","))
}

exit 0
