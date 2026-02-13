param(
    [switch]$IncludeBacktest,
    [switch]$StrictLogCheck,
    [switch]$NoStrictLogCheck,
    [switch]$StrictExecutionParity,
    [string]$SnapshotPath = "build/Release/state/snapshot_state.json",
    [string]$LegacyStatePath = "build/Release/state/state.json",
    [string]$JournalPath = "build/Release/state/event_journal.jsonl",
    [string]$LogDir = "build/Release/logs",
    [string]$RecoveryOutputJson = "build/Release/logs/recovery_e2e_report_strict.json",
    [string]$RecoveryStateValidationJson = "build/Release/logs/recovery_state_validation_strict.json",
    [string]$ReplayReconcileOutputJson = "build/Release/logs/replay_reconcile_diff_report.json",
    [string]$ExecutionParityOutputJson = "build/Release/logs/execution_parity_report.json",
    [string]$BacktestOutputJson = "build/Release/logs/readiness_report.json",
    [string]$OutputJson = "build/Release/logs/operational_readiness_report.json"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($StrictLogCheck.IsPresent -and $NoStrictLogCheck.IsPresent) {
    throw "Invalid option combination: -StrictLogCheck and -NoStrictLogCheck cannot be used together."
}

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

$resolvedOutputPath = Resolve-RepoPath $OutputJson
$resolvedRecoveryOutputPath = Resolve-RepoPath $RecoveryOutputJson
$resolvedRecoveryStateValidationPath = Resolve-RepoPath $RecoveryStateValidationJson
$resolvedReplayReconcileOutputPath = Resolve-RepoPath $ReplayReconcileOutputJson
$resolvedExecutionParityOutputPath = Resolve-RepoPath $ExecutionParityOutputJson
$resolvedBacktestOutputPath = Resolve-RepoPath $BacktestOutputJson

$recoveryScriptPath = Resolve-RepoPath "scripts/validate_recovery_e2e.ps1"
if (-not (Test-Path $recoveryScriptPath)) {
    throw "Missing script: $recoveryScriptPath"
}

$readinessScriptPath = Resolve-RepoPath "scripts/validate_readiness.ps1"
if ($IncludeBacktest.IsPresent -and -not (Test-Path $readinessScriptPath)) {
    throw "Missing script: $readinessScriptPath"
}
$replayReconcileScriptPath = Resolve-RepoPath "scripts/validate_replay_reconcile_diff.ps1"
if (-not (Test-Path $replayReconcileScriptPath)) {
    throw "Missing script: $replayReconcileScriptPath"
}
$executionParityScriptPath = Resolve-RepoPath "scripts/validate_execution_parity.ps1"
if (-not (Test-Path $executionParityScriptPath)) {
    throw "Missing script: $executionParityScriptPath"
}

$recoveryArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $recoveryScriptPath,
    "-SnapshotPath", (Resolve-RepoPath $SnapshotPath),
    "-LegacyStatePath", (Resolve-RepoPath $LegacyStatePath),
    "-JournalPath", (Resolve-RepoPath $JournalPath),
    "-LogDir", (Resolve-RepoPath $LogDir),
    "-OutputJson", $resolvedRecoveryOutputPath,
    "-StateValidationJson", $resolvedRecoveryStateValidationPath
)
$strictLogCheckEnabled = $true
if ($NoStrictLogCheck.IsPresent) {
    $strictLogCheckEnabled = $false
}
if ($StrictLogCheck.IsPresent) {
    $strictLogCheckEnabled = $true
}
if ($strictLogCheckEnabled) {
    $recoveryArgs += "-StrictLogCheck"
}

& powershell @recoveryArgs
$recoveryExit = $LASTEXITCODE

$replayReconcileArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $replayReconcileScriptPath,
    "-StateValidationJson", $resolvedRecoveryStateValidationPath,
    "-RecoveryE2EJson", $resolvedRecoveryOutputPath,
    "-LogDir", (Resolve-RepoPath $LogDir),
    "-OutputJson", $resolvedReplayReconcileOutputPath
)
if ($strictLogCheckEnabled) {
    $replayReconcileArgs += "-Strict"
}
& powershell @replayReconcileArgs
$replayReconcileExit = $LASTEXITCODE

$executionParityArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $executionParityScriptPath,
    "-OutputJson", $resolvedExecutionParityOutputPath
)
if ($StrictExecutionParity.IsPresent) {
    $executionParityArgs += "-Strict"
}
& powershell @executionParityArgs
$executionParityExit = $LASTEXITCODE

$backtestExit = $null
if ($IncludeBacktest.IsPresent) {
    $backtestArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $readinessScriptPath,
        "-OutputJson", $resolvedBacktestOutputPath
    )
    & powershell @backtestArgs
    $backtestExit = $LASTEXITCODE
}

$recoveryReport = Load-JsonOrNull $resolvedRecoveryOutputPath
$recoveryStateValidationReport = Load-JsonOrNull $resolvedRecoveryStateValidationPath
$replayReconcileReport = Load-JsonOrNull $resolvedReplayReconcileOutputPath
$executionParityReport = Load-JsonOrNull $resolvedExecutionParityOutputPath
$backtestReport = $null
if ($IncludeBacktest.IsPresent) {
    $backtestReport = Load-JsonOrNull $resolvedBacktestOutputPath
}

$errors = @()
if ($recoveryExit -ne 0) {
    $errors += "recovery_validation_failed"
}
if ($replayReconcileExit -ne 0) {
    $errors += "replay_reconcile_validation_failed"
}
if ($executionParityExit -ne 0) {
    $errors += "execution_parity_validation_failed"
}
if ($IncludeBacktest.IsPresent -and $backtestExit -ne 0) {
    $errors += "backtest_readiness_failed"
}

$report = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    options = [ordered]@{
        include_backtest = [bool]$IncludeBacktest
        strict_log_check = [bool]$strictLogCheckEnabled
        strict_execution_parity = [bool]$StrictExecutionParity
    }
    checks = [ordered]@{
        recovery_e2e_passed = ($recoveryExit -eq 0)
        replay_reconcile_diff_passed = ($replayReconcileExit -eq 0)
        execution_parity_passed = ($executionParityExit -eq 0)
        backtest_readiness_executed = [bool]$IncludeBacktest
        backtest_readiness_passed = if ($IncludeBacktest.IsPresent) { $backtestExit -eq 0 } else { $null }
    }
    artifacts = [ordered]@{
        recovery_report = $resolvedRecoveryOutputPath
        recovery_state_validation_report = $resolvedRecoveryStateValidationPath
        replay_reconcile_report = $resolvedReplayReconcileOutputPath
        execution_parity_report = $resolvedExecutionParityOutputPath
        backtest_readiness_report = if ($IncludeBacktest.IsPresent) { $resolvedBacktestOutputPath } else { $null }
    }
    recovery = $recoveryReport
    recovery_state_validation = $recoveryStateValidationReport
    replay_reconcile = $replayReconcileReport
    execution_parity = $executionParityReport
    backtest = $backtestReport
    errors = $errors
}

$outputDir = Split-Path -Parent $resolvedOutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -Path $outputDir -ItemType Directory -Force | Out-Null
}

($report | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedOutputPath -Encoding UTF8

if ($errors.Count -gt 0) {
    Write-Host "[OperationalReadiness] FAILED - see $resolvedOutputPath"
    exit 1
}

Write-Host "[OperationalReadiness] PASSED - see $resolvedOutputPath"
exit 0
