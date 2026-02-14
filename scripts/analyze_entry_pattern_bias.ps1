param(
    [string]$ExePath = ".\build\Release\AutoLifeTrading.exe",
    [string]$ConfigPath = ".\build\Release\config\config.json",
    [string]$GateReportJson = ".\build\Release\logs\profitability_gate_report_realdata.json",
    [string[]]$DataDirs = @(
        ".\data\backtest",
        ".\data\backtest_curated",
        ".\data\backtest_real"
    ),
    [string]$ProfileId = "core_full",
    [int]$MaxDatasets = 12,
    [int]$MinPatternTrades = 6,
    [switch]$IncludeOnlyLossDatasets,
    [string]$OutputWinningCsv = ".\build\Release\logs\entry_patterns_winning.csv",
    [string]$OutputLosingCsv = ".\build\Release\logs\entry_patterns_losing.csv",
    [string]$OutputRecommendationJson = ".\build\Release\logs\entry_pattern_recommendations.json"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-OrThrow {
    param([string]$PathValue, [string]$Label)
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
    param($ObjectValue, [string]$Name, $Value)
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

function Ensure-CoreFullFlags {
    param($ConfigObject)
    if ($null -eq $ConfigObject.trading) {
        $ConfigObject | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
    }
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_plane_bridge" -Value $true
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_policy_plane" -Value $true
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_risk_plane" -Value $true
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "enable_core_execution_plane" -Value $true
}

function Invoke-BacktestJson {
    param([string]$ExeFile, [string]$DatasetPath)
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

function Sum-Property {
    param(
        [object[]]$Rows,
        [string]$PropertyName
    )
    if ($null -eq $Rows -or $Rows.Count -eq 0) {
        return 0.0
    }
    $sum = 0.0
    foreach ($r in $Rows) {
        if ($null -eq $r) {
            continue
        }
        $prop = $r.PSObject.Properties[$PropertyName]
        if ($null -eq $prop) {
            continue
        }
        $sum += [double]$prop.Value
    }
    return $sum
}

$resolvedExe = Resolve-OrThrow -PathValue $ExePath -Label "Executable"
$resolvedConfig = Resolve-OrThrow -PathValue $ConfigPath -Label "Runtime config"
$resolvedGateReport = Resolve-OrThrow -PathValue $GateReportJson -Label "Gate report"

$resolvedOutputWinningCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputWinningCsv))
$resolvedOutputLosingCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputLosingCsv))
$resolvedOutputRecommendationJson = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputRecommendationJson))
Ensure-ParentDirectory -PathValue $resolvedOutputWinningCsv
Ensure-ParentDirectory -PathValue $resolvedOutputLosingCsv
Ensure-ParentDirectory -PathValue $resolvedOutputRecommendationJson

$datasetLookup = @{}
foreach ($dirPath in $DataDirs) {
    $resolvedDir = Resolve-Path -Path $dirPath -ErrorAction SilentlyContinue
    if ($null -eq $resolvedDir) {
        continue
    }
    $files = Get-ChildItem -Path $resolvedDir.Path -File -Filter *.csv -ErrorAction SilentlyContinue
    foreach ($f in $files) {
        $k = $f.Name.ToLowerInvariant()
        if (-not $datasetLookup.ContainsKey($k)) {
            $datasetLookup[$k] = $f.FullName
        }
    }
}
if ($datasetLookup.Count -eq 0) {
    throw "No dataset files found in DataDirs."
}

$report = Get-Content -Raw -Path $resolvedGateReport -Encoding UTF8 | ConvertFrom-Json
$rows = @(
    $report.matrix_rows |
        Where-Object { $_.profile_id -eq $ProfileId -and [double]$_.total_trades -gt 0 }
)
if ($rows.Count -eq 0) {
    throw "No matrix rows found for profile_id=$ProfileId"
}

$selectedRows = @()
if ($IncludeOnlyLossDatasets.IsPresent) {
    $selectedRows = @(
        $rows |
            Where-Object { [double]$_.total_profit_krw -lt 0.0 } |
            Sort-Object { [double]$_.total_profit_krw } |
            Select-Object -First $MaxDatasets
    )
} else {
    $lossCount = [Math]::Max(1, [Math]::Floor($MaxDatasets / 2))
    $gainCount = [Math]::Max(1, $MaxDatasets - $lossCount)
    $lossRows = @(
        $rows |
            Where-Object { [double]$_.total_profit_krw -lt 0.0 } |
            Sort-Object { [double]$_.total_profit_krw } |
            Select-Object -First $lossCount
    )
    $gainRows = @(
        $rows |
            Where-Object { [double]$_.total_profit_krw -gt 0.0 } |
            Sort-Object { [double]$_.total_profit_krw } -Descending |
            Select-Object -First $gainCount
    )
    $selectedRows = @($lossRows + $gainRows)
}

if ($selectedRows.Count -lt $MaxDatasets) {
    $remain = $MaxDatasets - $selectedRows.Count
    $used = @($selectedRows | ForEach-Object { [string]$_.dataset.ToLowerInvariant() })
    $fillRows = @(
        $rows |
            Where-Object { $used -notcontains ([string]$_.dataset).ToLowerInvariant() } |
            Sort-Object { [Math]::Abs([double]$_.total_profit_krw) } -Descending |
            Select-Object -First $remain
    )
    $selectedRows = @($selectedRows + $fillRows)
}

$selectedRows = @(
    $selectedRows |
        Sort-Object dataset -Unique |
        Select-Object -First $MaxDatasets
)
if ($selectedRows.Count -eq 0) {
    throw "No datasets selected."
}

$patternAgg = @{}
$datasetRunSummaries = New-Object "System.Collections.Generic.List[object]"
$missingDatasets = New-Object "System.Collections.Generic.List[string]"
$failedDatasets = New-Object "System.Collections.Generic.List[string]"

$originalConfigRaw = Get-Content -Raw -Path $resolvedConfig -Encoding UTF8
try {
    $cfg = $originalConfigRaw | ConvertFrom-Json
    Ensure-CoreFullFlags -ConfigObject $cfg
    ($cfg | ConvertTo-Json -Depth 32) | Set-Content -Path $resolvedConfig -Encoding UTF8

    foreach ($row in $selectedRows) {
        $datasetFile = [string]$row.dataset
        $datasetKey = $datasetFile.ToLowerInvariant()
        if (-not $datasetLookup.ContainsKey($datasetKey)) {
            $missingDatasets.Add($datasetFile) | Out-Null
            continue
        }
        $datasetPath = [string]$datasetLookup[$datasetKey]
        Write-Host ("[EntryPattern] Backtest: {0}" -f $datasetFile)

        $bt = Invoke-BacktestJson -ExeFile $resolvedExe -DatasetPath $datasetPath
        if ($null -eq $bt) {
            $failedDatasets.Add($datasetFile) | Out-Null
            continue
        }
        $datasetRunSummaries.Add([pscustomobject]@{
            dataset = $datasetFile
            total_trades = [int]$bt.total_trades
            total_profit = [double]$bt.total_profit
            expectancy_krw = [double]$bt.expectancy_krw
        }) | Out-Null

        if ($null -eq $bt.pattern_summaries) {
            continue
        }
        foreach ($p in $bt.pattern_summaries) {
            $key = ([string]$p.strategy_name) + "|" + ([string]$p.regime) + "|" + ([string]$p.strength_bucket) + "|" + ([string]$p.expected_value_bucket) + "|" + ([string]$p.reward_risk_bucket)
            if (-not $patternAgg.ContainsKey($key)) {
                $patternAgg[$key] = [ordered]@{
                    strategy_name = [string]$p.strategy_name
                    regime = [string]$p.regime
                    strength_bucket = [string]$p.strength_bucket
                    expected_value_bucket = [string]$p.expected_value_bucket
                    reward_risk_bucket = [string]$p.reward_risk_bucket
                    total_trades = 0
                    winning_trades = 0
                    losing_trades = 0
                    total_profit = 0.0
                    gross_profit = 0.0
                    gross_loss_abs = 0.0
                }
            }
            $item = $patternAgg[$key]
            $item.total_trades += [int]$p.total_trades
            $item.winning_trades += [int]$p.winning_trades
            $item.losing_trades += [int]$p.losing_trades
            $item.total_profit += [double]$p.total_profit
            if ([double]$p.total_profit -gt 0.0) {
                $item.gross_profit += [double]$p.total_profit
            } elseif ([double]$p.total_profit -lt 0.0) {
                $item.gross_loss_abs += [Math]::Abs([double]$p.total_profit)
            }
        }
    }
}
finally {
    $originalConfigRaw | Set-Content -Path $resolvedConfig -Encoding UTF8
}

$allPatternRows = @()
foreach ($item in $patternAgg.Values) {
    $trades = [int]$item.total_trades
    if ($trades -le 0) {
        continue
    }
    $winRate = [double]$item.winning_trades / [double]$trades
    $avgProfit = [double]$item.total_profit / [double]$trades
    $pf = if ([double]$item.gross_loss_abs -gt 1e-9) { [double]$item.gross_profit / [double]$item.gross_loss_abs } else { 0.0 }
    $allPatternRows += [pscustomobject]@{
        strategy_name = $item.strategy_name
        regime = $item.regime
        strength_bucket = $item.strength_bucket
        expected_value_bucket = $item.expected_value_bucket
        reward_risk_bucket = $item.reward_risk_bucket
        total_trades = $trades
        winning_trades = [int]$item.winning_trades
        losing_trades = [int]$item.losing_trades
        win_rate = [Math]::Round($winRate, 4)
        total_profit = [Math]::Round([double]$item.total_profit, 4)
        avg_profit_krw = [Math]::Round($avgProfit, 4)
        profit_factor = [Math]::Round($pf, 4)
    }
}

$winningRows = @(
    $allPatternRows |
        Where-Object {
            [int]$_.total_trades -ge $MinPatternTrades -and
            [double]$_.avg_profit_krw -gt 0.0 -and
            [double]$_.win_rate -ge 0.55 -and
            [double]$_.profit_factor -ge 1.05
        } |
        Sort-Object `
            @{ Expression = { [int]$_.total_trades }; Descending = $true }, `
            @{ Expression = { [double]$_.avg_profit_krw }; Descending = $true }
)
$losingRows = @(
    $allPatternRows |
        Where-Object {
            [int]$_.total_trades -ge $MinPatternTrades -and
            [double]$_.avg_profit_krw -lt 0.0 -and
            ([double]$_.win_rate -le 0.45 -or [double]$_.profit_factor -lt 0.95)
        } |
        Sort-Object `
            @{ Expression = { [int]$_.total_trades }; Descending = $true }, `
            @{ Expression = { [double]$_.avg_profit_krw }; Descending = $false }
)

$winningRows | Export-Csv -Path $resolvedOutputWinningCsv -NoTypeInformation -Encoding UTF8
$losingRows | Export-Csv -Path $resolvedOutputLosingCsv -NoTypeInformation -Encoding UTF8

$recommendations = New-Object "System.Collections.Generic.List[object]"
$grouped = $allPatternRows | Group-Object strategy_name, regime
foreach ($g in $grouped) {
    $rowsInGroup = @($g.Group)
    $groupTrades = [int](Sum-Property -Rows $rowsInGroup -PropertyName "total_trades")
    if ($groupTrades -le 0) {
        continue
    }

    $sumProfit = [double](Sum-Property -Rows $rowsInGroup -PropertyName "total_profit")
    $groupAvgProfit = $sumProfit / [double]$groupTrades

    $lowRows = @($rowsInGroup | Where-Object { $_.strength_bucket -eq "strength_low" -or $_.strength_bucket -eq "strength_mid" })
    $highRows = @($rowsInGroup | Where-Object { $_.strength_bucket -eq "strength_high" })
    $evNegRows = @($rowsInGroup | Where-Object { $_.expected_value_bucket -eq "ev_negative" })
    $rrLowRows = @($rowsInGroup | Where-Object { $_.reward_risk_bucket -eq "rr_low" })

    $lowTrades = [int](Sum-Property -Rows $lowRows -PropertyName "total_trades")
    $highTrades = [int](Sum-Property -Rows $highRows -PropertyName "total_trades")
    $lowAvg = if ($lowTrades -gt 0) { [double](Sum-Property -Rows $lowRows -PropertyName "total_profit") / [double]$lowTrades } else { 0.0 }
    $highAvg = if ($highTrades -gt 0) { [double](Sum-Property -Rows $highRows -PropertyName "total_profit") / [double]$highTrades } else { 0.0 }
    $evNegShare = [double](Sum-Property -Rows $evNegRows -PropertyName "total_trades") / [double]$groupTrades
    $rrLowShare = [double](Sum-Property -Rows $rrLowRows -PropertyName "total_trades") / [double]$groupTrades

    $minStrength = 0.50
    if ($lowTrades -ge [Math]::Max(3, [Math]::Floor($MinPatternTrades / 2)) -and $lowAvg -lt 0.0 -and $highTrades -ge 3 -and $highAvg -gt 0.0) {
        $minStrength = 0.70
    } elseif ($lowAvg -lt 0.0) {
        $minStrength = 0.62
    } elseif ($evNegShare -ge 0.35) {
        $minStrength = 0.60
    }

    $rrAdd = 0.0
    if ($rrLowShare -ge 0.50) { $rrAdd += 0.20 }
    elseif ($rrLowShare -ge 0.30) { $rrAdd += 0.10 }
    if ($evNegShare -ge 0.45) { $rrAdd += 0.08 }
    elseif ($evNegShare -ge 0.30) { $rrAdd += 0.04 }

    $edgeAdd = 0.0
    if ($evNegShare -ge 0.45) { $edgeAdd += 0.00040 }
    elseif ($evNegShare -ge 0.30) { $edgeAdd += 0.00020 }
    if ($rrLowShare -ge 0.50) { $edgeAdd += 0.00020 }

    $recommendBlock = ($groupTrades -ge 12 -and $groupAvgProfit -lt -15.0)
    $confidence = if ($groupTrades -ge 40) { "high" } elseif ($groupTrades -ge 18) { "medium" } else { "low" }

    $parts = [string]$g.Name -split ",\s*"
    $strategy = if ($parts.Length -gt 0) { $parts[0] } else { "" }
    $regime = if ($parts.Length -gt 1) { $parts[1] } else { "" }

    $recommendations.Add([pscustomobject]@{
        strategy_name = $strategy
        regime = $regime
        trades = $groupTrades
        avg_profit_krw = [Math]::Round($groupAvgProfit, 4)
        low_strength_avg_profit_krw = [Math]::Round($lowAvg, 4)
        high_strength_avg_profit_krw = [Math]::Round($highAvg, 4)
        ev_negative_share = [Math]::Round($evNegShare, 4)
        rr_low_share = [Math]::Round($rrLowShare, 4)
        recommended_min_strength = [Math]::Round($minStrength, 2)
        recommended_rr_add = [Math]::Round($rrAdd, 2)
        recommended_edge_add = [Math]::Round($edgeAdd, 6)
        recommend_block = [bool]$recommendBlock
        confidence = $confidence
    }) | Out-Null
}

$recommendationPayload = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    profile_id = $ProfileId
    max_datasets = $MaxDatasets
    min_pattern_trades = $MinPatternTrades
    datasets_analyzed = @($selectedRows | ForEach-Object { [string]$_.dataset })
    missing_datasets = @($missingDatasets.ToArray())
    failed_datasets = @($failedDatasets.ToArray())
    dataset_run_summaries = @($datasetRunSummaries.ToArray())
    winning_pattern_count = @($winningRows).Count
    losing_pattern_count = @($losingRows).Count
    recommendations = @(
        $recommendations |
            Sort-Object `
                @{ Expression = { [int]$_.trades }; Descending = $true }, `
                @{ Expression = { [double]$_.avg_profit_krw }; Descending = $true }
    )
}
($recommendationPayload | ConvertTo-Json -Depth 10) | Set-Content -Path $resolvedOutputRecommendationJson -Encoding UTF8

Write-Host "[EntryPattern] Completed"
Write-Host "winning_csv=$resolvedOutputWinningCsv"
Write-Host "losing_csv=$resolvedOutputLosingCsv"
Write-Host "recommendation_json=$resolvedOutputRecommendationJson"
