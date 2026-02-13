param(
    [string]$StateValidationJson = "build/Release/logs/recovery_state_validation_strict.json",
    [string]$RecoveryE2EJson = "build/Release/logs/recovery_e2e_report_strict.json",
    [string]$LogDir = "build/Release/logs",
    [string]$LogPath = "",
    [string]$OutputJson = "build/Release/logs/replay_reconcile_diff_report.json",
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

function Load-JsonOrNull {
    param([string]$PathValue)
    if (-not (Test-Path $PathValue)) {
        return $null
    }
    try {
        return (Get-Content -Raw $PathValue | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function To-StringArray {
    param([object]$Values)
    if ($null -eq $Values) {
        return @()
    }
    if ($Values -is [System.Array]) {
        return @($Values | ForEach-Object { [string]$_ })
    }
    return @([string]$Values)
}

$resolvedStateValidationPath = Resolve-RepoPath $StateValidationJson
$resolvedRecoveryE2EPath = Resolve-RepoPath $RecoveryE2EJson
$resolvedLogDir = Resolve-RepoPath $LogDir
$resolvedOutputPath = Resolve-RepoPath $OutputJson
$resolvedLogPath = if ([string]::IsNullOrWhiteSpace($LogPath)) { "" } else { Resolve-RepoPath $LogPath }

if ([string]::IsNullOrWhiteSpace($resolvedLogPath)) {
    if (Test-Path $resolvedLogDir) {
        $latestLog = Get-ChildItem -Path $resolvedLogDir -File -Filter "autolife*.log" |
            Sort-Object -Property LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $latestLog) {
            $resolvedLogPath = $latestLog.FullName
        }
    }
}

$stateValidation = Load-JsonOrNull $resolvedStateValidationPath
$recoveryE2E = Load-JsonOrNull $resolvedRecoveryE2EPath

$predictedOpen = $null
if ($null -ne $stateValidation -and $stateValidation.metrics.PSObject.Properties.Name -contains "predicted_open_positions_after_replay") {
    $predictedOpen = [int]$stateValidation.metrics.predicted_open_positions_after_replay
}

$logExists = (-not [string]::IsNullOrWhiteSpace($resolvedLogPath)) -and (Test-Path $resolvedLogPath)
$summaryLine = $null
$summary = $null

if ($logExists) {
    $lines = Microsoft.PowerShell.Management\Get-Content $resolvedLogPath
    $summaryCandidates = @($lines | Where-Object { ([string]$_) -like "*Account state sync summary:*" })
    if ($summaryCandidates.Count -gt 0) {
        $summaryLine = [string]$summaryCandidates[-1]
        $pattern = "Account state sync summary: wallet_markets=(\d+), reconcile_candidates=(\d+), restored_positions=(\d+), external_closes=(\d+)"
        $match = [regex]::Match($summaryLine, $pattern)
        if ($match.Success) {
            $summary = [ordered]@{
                wallet_markets = [int]$match.Groups[1].Value
                reconcile_candidates = [int]$match.Groups[2].Value
                restored_positions = [int]$match.Groups[3].Value
                external_closes = [int]$match.Groups[4].Value
            }
        }
    }
}

$checks = [ordered]@{
    state_validation_available = ($null -ne $stateValidation)
    recovery_e2e_available = ($null -ne $recoveryE2E)
    log_available = $logExists
    sync_summary_available = ($null -ne $summary)
    predicted_open_available = ($null -ne $predictedOpen)
    candidates_match_predicted = $false
    partition_consistent = $false
}

$metrics = [ordered]@{
    predicted_open_positions_after_replay = if ($null -ne $predictedOpen) { $predictedOpen } else { $null }
    reconcile_candidates = if ($null -ne $summary) { $summary.reconcile_candidates } else { $null }
    restored_positions = if ($null -ne $summary) { $summary.restored_positions } else { $null }
    external_closes = if ($null -ne $summary) { $summary.external_closes } else { $null }
    wallet_markets = if ($null -ne $summary) { $summary.wallet_markets } else { $null }
    partition_delta = $null
}

if ($null -ne $summary -and $null -ne $predictedOpen) {
    $checks.candidates_match_predicted = ($summary.reconcile_candidates -eq $predictedOpen)
    $partitionDelta = ($summary.restored_positions + $summary.external_closes) - $predictedOpen
    $metrics.partition_delta = $partitionDelta
    $checks.partition_consistent = ($partitionDelta -eq 0)
}

$warnings = @()
if (-not $checks.state_validation_available) {
    $warnings += "state_validation_json_missing_or_invalid"
}
if (-not $checks.recovery_e2e_available) {
    $warnings += "recovery_e2e_json_missing_or_invalid"
}
if (-not $checks.log_available) {
    $warnings += "log_not_found"
}
if ($checks.log_available -and -not $checks.sync_summary_available) {
    $warnings += "sync_summary_log_missing"
}
if ($checks.sync_summary_available -and -not $checks.predicted_open_available) {
    $warnings += "predicted_open_positions_missing"
}
if ($checks.sync_summary_available -and $checks.predicted_open_available -and -not $checks.candidates_match_predicted) {
    $warnings += "reconcile_candidates_mismatch"
}
if ($checks.sync_summary_available -and $checks.predicted_open_available -and -not $checks.partition_consistent) {
    $warnings += "replay_reconcile_partition_mismatch"
}

$errors = @()
if ($Strict.IsPresent) {
    if (-not $checks.log_available) {
        $errors += "strict_failed_no_log"
    }
    if (-not $checks.sync_summary_available) {
        $errors += "strict_failed_no_sync_summary"
    }
    if (-not $checks.predicted_open_available) {
        $errors += "strict_failed_no_predicted_open_positions"
    }
    if (-not $checks.candidates_match_predicted) {
        $errors += "strict_failed_candidates_mismatch"
    }
    if (-not $checks.partition_consistent) {
        $errors += "strict_failed_partition_mismatch"
    }
}

$replayLogEvidence = @()
$replayNoopEvidence = @()
if ($null -ne $recoveryE2E -and $recoveryE2E.evidence.PSObject.Properties.Name -contains "replay_applied") {
    $replayLogEvidence = @(To-StringArray $recoveryE2E.evidence.replay_applied)
}
if ($null -ne $recoveryE2E -and $recoveryE2E.evidence.PSObject.Properties.Name -contains "replay_noop") {
    $replayNoopEvidence = @(To-StringArray $recoveryE2E.evidence.replay_noop)
}

$report = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    inputs = [ordered]@{
        state_validation_json = $resolvedStateValidationPath
        recovery_e2e_json = $resolvedRecoveryE2EPath
        log_path = $resolvedLogPath
    }
    checks = $checks
    metrics = $metrics
    evidence = [ordered]@{
        sync_summary_line = $summaryLine
        replay_log = [string[]]$replayLogEvidence
        replay_noop_log = [string[]]$replayNoopEvidence
    }
    warnings = $warnings
    errors = $errors
}

$outputDir = Split-Path -Parent $resolvedOutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -Path $outputDir -ItemType Directory -Force | Out-Null
}

($report | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedOutputPath -Encoding UTF8

if ($errors.Count -gt 0) {
    Write-Host "[ReplayReconcileDiff] FAILED - see $resolvedOutputPath"
    exit 1
}

if ($warnings.Count -gt 0) {
    Write-Host "[ReplayReconcileDiff] PASSED with warnings - see $resolvedOutputPath"
} else {
    Write-Host "[ReplayReconcileDiff] PASSED - see $resolvedOutputPath"
}
exit 0
