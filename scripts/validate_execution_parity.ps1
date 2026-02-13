param(
    [string]$LiveExecutionUpdatesPath = "build/Release/logs/execution_updates_live.jsonl",
    [string]$BacktestExecutionUpdatesPath = "build/Release/logs/execution_updates_backtest.jsonl",
    [string]$OutputJson = "build/Release/logs/execution_parity_report.json",
    [switch]$Strict
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$PathValue)
    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return $PathValue
    }
    return Join-Path (Get-Location) $PathValue
}

function New-StringSet {
    return New-Object "System.Collections.Generic.HashSet[string]"
}

function To-SortedArray {
    param([System.Collections.Generic.HashSet[string]]$SetValue)
    return @($SetValue | ForEach-Object { [string]$_ } | Sort-Object)
}

function Read-ExecutionUpdates {
    param(
        [string]$PathValue,
        [string[]]$ExpectedKeys,
        [string[]]$AllowedSides,
        [string[]]$AllowedStatuses
    )

    $exists = Test-Path $PathValue
    $report = [ordered]@{
        path = $PathValue
        exists = $exists
        total_lines = 0
        parsed_lines = 0
        parse_errors = 0
        missing_keys = @()
        unexpected_keys = @()
        invalid_side_values = @()
        invalid_status_values = @()
        key_signature = ""
        schema_valid = $false
        sample = $null
    }

    if (-not $exists) {
        return [pscustomobject]$report
    }

    $allKeys = New-StringSet
    $missingKeys = New-StringSet
    $unexpectedKeys = New-StringSet
    $invalidSides = New-StringSet
    $invalidStatuses = New-StringSet

    $lines = Get-Content -Path $PathValue
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $report.total_lines++

        $row = $null
        try {
            $row = $line | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $report.parse_errors++
            continue
        }

        $report.parsed_lines++
        if ($null -eq $report.sample) {
            $report.sample = $row
        }

        $props = @($row.PSObject.Properties.Name)
        foreach ($prop in $props) {
            [void]$allKeys.Add([string]$prop)
        }

        foreach ($expectedKey in $ExpectedKeys) {
            if ($props -notcontains $expectedKey) {
                [void]$missingKeys.Add($expectedKey)
            }
        }

        foreach ($prop in $props) {
            if ($ExpectedKeys -notcontains $prop) {
                [void]$unexpectedKeys.Add($prop)
            }
        }

        if ($props -contains "side") {
            $sideValue = [string]$row.side
            if ($AllowedSides -notcontains $sideValue) {
                [void]$invalidSides.Add($sideValue)
            }
        }

        if ($props -contains "status") {
            $statusValue = [string]$row.status
            if ($AllowedStatuses -notcontains $statusValue) {
                [void]$invalidStatuses.Add($statusValue)
            }
        }
    }

    $sortedKeys = To-SortedArray $allKeys
    $report.key_signature = ($sortedKeys -join ",")
    $report.missing_keys = @(To-SortedArray $missingKeys)
    $report.unexpected_keys = @(To-SortedArray $unexpectedKeys)
    $report.invalid_side_values = @(To-SortedArray $invalidSides)
    $report.invalid_status_values = @(To-SortedArray $invalidStatuses)
    $report.schema_valid = (
        $report.parse_errors -eq 0 -and
        $report.parsed_lines -gt 0 -and
        @($report.missing_keys).Count -eq 0 -and
        @($report.unexpected_keys).Count -eq 0 -and
        @($report.invalid_side_values).Count -eq 0 -and
        @($report.invalid_status_values).Count -eq 0
    )

    return [pscustomobject]$report
}

$expectedKeys = @(
    "ts_ms",
    "source",
    "event",
    "order_id",
    "market",
    "side",
    "status",
    "filled_volume",
    "order_volume",
    "avg_price",
    "strategy_name",
    "terminal"
)
$allowedSides = @("BUY", "SELL")
$allowedStatuses = @("PENDING", "SUBMITTED", "FILLED", "PARTIALLY_FILLED", "CANCELLED", "REJECTED")

$resolvedLivePath = Resolve-RepoPath $LiveExecutionUpdatesPath
$resolvedBacktestPath = Resolve-RepoPath $BacktestExecutionUpdatesPath
$resolvedOutputPath = Resolve-RepoPath $OutputJson

$live = Read-ExecutionUpdates -PathValue $resolvedLivePath -ExpectedKeys $expectedKeys -AllowedSides $allowedSides -AllowedStatuses $allowedStatuses
$backtest = Read-ExecutionUpdates -PathValue $resolvedBacktestPath -ExpectedKeys $expectedKeys -AllowedSides $allowedSides -AllowedStatuses $allowedStatuses

$checks = [ordered]@{
    live_file_available = [bool]$live.exists
    backtest_file_available = [bool]$backtest.exists
    live_rows_available = ($live.parsed_lines -gt 0)
    backtest_rows_available = ($backtest.parsed_lines -gt 0)
    live_schema_valid = [bool]$live.schema_valid
    backtest_schema_valid = [bool]$backtest.schema_valid
    both_have_rows = (($live.parsed_lines -gt 0) -and ($backtest.parsed_lines -gt 0))
    schema_compatible = $false
}

if ($checks.both_have_rows) {
    $checks.schema_compatible = ($live.key_signature -eq $backtest.key_signature)
}

$warnings = @()
$errors = @()

if (-not $checks.live_file_available) {
    $warnings += "live_execution_updates_missing"
}
if (-not $checks.backtest_file_available) {
    $warnings += "backtest_execution_updates_missing"
}
if ($checks.live_file_available -and -not $checks.live_rows_available) {
    $warnings += "live_execution_updates_empty"
}
if ($checks.backtest_file_available -and -not $checks.backtest_rows_available) {
    $warnings += "backtest_execution_updates_empty"
}

if ($live.parse_errors -gt 0) {
    $errors += "live_execution_updates_parse_error"
}
if ($backtest.parse_errors -gt 0) {
    $errors += "backtest_execution_updates_parse_error"
}
if ($checks.live_rows_available -and -not $checks.live_schema_valid) {
    $errors += "live_execution_updates_schema_invalid"
}
if ($checks.backtest_rows_available -and -not $checks.backtest_schema_valid) {
    $errors += "backtest_execution_updates_schema_invalid"
}
if ($checks.both_have_rows -and -not $checks.schema_compatible) {
    $errors += "execution_update_schema_mismatch"
}

if ($Strict.IsPresent) {
    if (-not $checks.live_file_available) {
        $errors += "strict_failed_live_execution_updates_missing"
    }
    if (-not $checks.backtest_file_available) {
        $errors += "strict_failed_backtest_execution_updates_missing"
    }
    if (-not $checks.live_rows_available) {
        $errors += "strict_failed_live_execution_updates_empty"
    }
    if (-not $checks.backtest_rows_available) {
        $errors += "strict_failed_backtest_execution_updates_empty"
    }
    if (-not $checks.schema_compatible) {
        $errors += "strict_failed_execution_schema_mismatch"
    }
}

$report = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    inputs = [ordered]@{
        live_execution_updates = $resolvedLivePath
        backtest_execution_updates = $resolvedBacktestPath
    }
    expected_schema = [ordered]@{
        keys = [string[]]$expectedKeys
        side_values = [string[]]$allowedSides
        status_values = [string[]]$allowedStatuses
    }
    checks = $checks
    live = $live
    backtest = $backtest
    warnings = $warnings
    errors = $errors
}

$outputDir = Split-Path -Parent $resolvedOutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -Path $outputDir -ItemType Directory -Force | Out-Null
}

($report | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedOutputPath -Encoding UTF8

if ($errors.Count -gt 0) {
    Write-Host "[ExecutionParity] FAILED - see $resolvedOutputPath"
    exit 1
}

if ($warnings.Count -gt 0) {
    Write-Host "[ExecutionParity] PASSED with warnings - see $resolvedOutputPath"
} else {
    Write-Host "[ExecutionParity] PASSED - see $resolvedOutputPath"
}
exit 0
