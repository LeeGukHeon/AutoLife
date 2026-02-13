param(
    [string]$SnapshotPath = "build/Release/state/snapshot_state.json",
    [string]$LegacyStatePath = "build/Release/state/state.json",
    [string]$JournalPath = "build/Release/state/event_journal.jsonl",
    [string]$LogDir = "build/Release/logs",
    [string]$LogPath = "",
    [string]$OutputJson = "build/Release/logs/recovery_e2e_report.json",
    [string]$StateValidationJson = "build/Release/logs/recovery_state_validation.json",
    [switch]$StrictLogCheck
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

function To-StringArrayTail {
    param(
        [object[]]$Values,
        [int]$Count = 3
    )

    if ($null -eq $Values) {
        return @()
    }

    $tail = @($Values | Select-Object -Last $Count)
    return @($tail | ForEach-Object { [string]$_ })
}

$resolvedSnapshotPath = Resolve-RepoPath $SnapshotPath
$resolvedLegacyPath = Resolve-RepoPath $LegacyStatePath
$resolvedJournalPath = Resolve-RepoPath $JournalPath
$resolvedLogDir = Resolve-RepoPath $LogDir
$resolvedOutputPath = Resolve-RepoPath $OutputJson
$resolvedStateValidationPath = Resolve-RepoPath $StateValidationJson
$resolvedLogPath = if ([string]::IsNullOrWhiteSpace($LogPath)) { "" } else { Resolve-RepoPath $LogPath }

$stateValidationScript = Resolve-RepoPath "scripts/validate_recovery_state.ps1"
if (-not (Test-Path $stateValidationScript)) {
    throw "Missing script: $stateValidationScript"
}

$stateValidationExit = 0
try {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $stateValidationScript `
        -SnapshotPath $resolvedSnapshotPath `
        -LegacyStatePath $resolvedLegacyPath `
        -JournalPath $resolvedJournalPath `
        -OutputJson $resolvedStateValidationPath
    $stateValidationExit = $LASTEXITCODE
} catch {
    $stateValidationExit = 1
}

$stateValidation = $null
if (Test-Path $resolvedStateValidationPath) {
    try {
        $stateValidation = Get-Content $resolvedStateValidationPath -Raw | ConvertFrom-Json
    } catch {
        $stateValidation = $null
    }
}

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

$logExists = (-not [string]::IsNullOrWhiteSpace($resolvedLogPath)) -and (Test-Path $resolvedLogPath)
$logMatches = [ordered]@{
    snapshot_loaded = @()
    replay_applied = @()
    replay_noop = @()
    reconcile_completed = @()
}

if ($logExists) {
    $lines = Microsoft.PowerShell.Management\Get-Content $resolvedLogPath
    foreach ($line in $lines) {
        $text = [string]$line
        if ($text -like "*State snapshot loaded:*") {
            $logMatches.snapshot_loaded += $text
        }
        if ($text -like "*State restore: journal replay applied*") {
            $logMatches.replay_applied += $text
        }
        if ($text -like "*State restore: no replay events applied*") {
            $logMatches.replay_noop += $text
        }
        if ($text -like "*Account state synchronization completed*") {
            $logMatches.reconcile_completed += $text
        }
    }
}

$replayableCount = 0
if ($null -ne $stateValidation -and $stateValidation.metrics.PSObject.Properties.Name -contains "replayable_event_count") {
    $replayableCount = [int]$stateValidation.metrics.replayable_event_count
}

$checks = [ordered]@{
    state_validation_passed = ($stateValidationExit -eq 0)
    log_available = $logExists
    log_has_snapshot_loaded = ($logMatches.snapshot_loaded.Count -gt 0)
    log_has_replay_evidence = (($logMatches.replay_applied.Count -gt 0) -or ($logMatches.replay_noop.Count -gt 0))
    log_has_reconcile_completed = ($logMatches.reconcile_completed.Count -gt 0)
}

$warnings = @()
if (-not $checks.log_available) {
    $warnings += "log_not_found"
}
if ($checks.log_available -and -not $checks.log_has_snapshot_loaded) {
    $warnings += "snapshot_load_log_missing"
}
if ($checks.log_available -and -not $checks.log_has_replay_evidence) {
    $warnings += "replay_log_missing"
}
if ($checks.log_available -and -not $checks.log_has_reconcile_completed) {
    $warnings += "reconcile_log_missing"
}

if ($replayableCount -gt 0 -and $checks.log_available -and -not $checks.log_has_replay_evidence) {
    $warnings += "replayable_events_without_replay_log"
}

$errors = @()
if (-not $checks.state_validation_passed) {
    $errors += "state_validation_failed"
}
if ($StrictLogCheck.IsPresent) {
    if (-not $checks.log_available) {
        $errors += "strict_log_check_failed_no_log"
    } elseif (-not $checks.log_has_snapshot_loaded -or -not $checks.log_has_replay_evidence -or -not $checks.log_has_reconcile_completed) {
        $errors += "strict_log_check_failed_incomplete_markers"
    }
}

$report = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    inputs = [ordered]@{
        snapshot_path = $resolvedSnapshotPath
        legacy_state_path = $resolvedLegacyPath
        journal_path = $resolvedJournalPath
        log_path = $resolvedLogPath
        state_validation_json = $resolvedStateValidationPath
    }
    checks = $checks
    metrics = [ordered]@{
        replayable_event_count = $replayableCount
        snapshot_loaded_log_count = $logMatches.snapshot_loaded.Count
        replay_applied_log_count = $logMatches.replay_applied.Count
        replay_noop_log_count = $logMatches.replay_noop.Count
        reconcile_completed_log_count = $logMatches.reconcile_completed.Count
    }
    evidence = [ordered]@{
        snapshot_loaded = @(To-StringArrayTail -Values $logMatches.snapshot_loaded -Count 3)
        replay_applied = @(To-StringArrayTail -Values $logMatches.replay_applied -Count 3)
        replay_noop = @(To-StringArrayTail -Values $logMatches.replay_noop -Count 3)
        reconcile_completed = @(To-StringArrayTail -Values $logMatches.reconcile_completed -Count 3)
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
    Write-Host "[RecoveryE2E] FAILED - see $resolvedOutputPath"
    exit 1
}

if ($warnings.Count -gt 0) {
    Write-Host "[RecoveryE2E] PASSED with warnings - see $resolvedOutputPath"
} else {
    Write-Host "[RecoveryE2E] PASSED - see $resolvedOutputPath"
}

exit 0
