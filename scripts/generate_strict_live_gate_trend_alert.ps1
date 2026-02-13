param(
    [string]$ExecutionParityReportPath = "build/Release/logs/execution_parity_report.json",
    [string]$OperationalReadinessReportPath = "build/Release/logs/operational_readiness_report.json",
    [string]$HistoryPath = "build/Release/logs/strict_live_gate_history.jsonl",
    [string]$DailySummaryJsonPath = "build/Release/logs/strict_live_gate_daily_summary.json",
    [string]$WeeklySummaryJsonPath = "build/Release/logs/strict_live_gate_weekly_summary.json",
    [string]$DailySummaryCsvPath = "build/Release/logs/strict_live_gate_daily_summary.csv",
    [string]$WeeklySummaryCsvPath = "build/Release/logs/strict_live_gate_weekly_summary.csv",
    [string]$AlertOutputJsonPath = "build/Release/logs/strict_live_gate_alert_report.json",
    [string]$ThresholdTuningOutputJsonPath = "build/Release/logs/strict_live_gate_threshold_tuning_report.json",
    [string]$ActionResponseOutputJsonPath = "build/Release/logs/strict_live_gate_action_response_report.json",
    [string]$GateProfile = "strict_live",
    [int]$ConsecutiveFailureThreshold = 2,
    [double]$WarningRatioThreshold = 0.30,
    [int]$WarningRatioLookbackDays = 7,
    [int]$WarningRatioMinSamples = 3,
    [int]$TuningLookbackDays = 28,
    [int]$TuningMinHistoryRuns = 14,
    [double]$ConsecutiveFailureAlertRateTarget = 0.10,
    [int]$ConsecutiveFailureThresholdMin = 2,
    [int]$ConsecutiveFailureThresholdMax = 5,
    [double]$WarningRatioQuantile = 0.85,
    [int]$WarningRatioTuningMinSamples = 7,
    [double]$WarningRatioThresholdMin = 0.10,
    [double]$WarningRatioThresholdMax = 0.80,
    [switch]$ApplyTunedThresholds,
    [ValidateSet("report-only", "safe-auto-execute")]
    [string]$ActionExecutionPolicy = "report-only",
    [switch]$EnableActionFeedbackLoop,
    [int]$FeedbackStabilizationMinHistoryRuns = 21,
    [int]$FeedbackStabilizationMinWarningSamples = 10,
    [int]$FeedbackWeeklyLookbackWeeks = 4,
    [int]$FeedbackWeeklyMinRunsPerWeek = 3,
    [double]$FeedbackWeeklySignalDriftThreshold = 0.20,
    [int]$FeedbackConsecutiveAdjustmentStep = 1,
    [int]$FeedbackConsecutiveAdjustmentCap = 2,
    [double]$FeedbackWarningRatioAdjustmentStep = 0.05,
    [double]$FeedbackWarningRatioAdjustmentCap = 0.10,
    [switch]$FailOnCriticalAlert
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

function Ensure-ParentDirectory {
    param([string]$PathValue)
    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path $parent)) {
        New-Item -Path $parent -ItemType Directory -Force | Out-Null
    }
}

function Get-ArrayCount {
    param($Value)
    if ($null -eq $Value) {
        return 0
    }
    if ($Value -is [string]) {
        if ([string]::IsNullOrWhiteSpace($Value)) {
            return 0
        }
        return 1
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        return @($Value).Count
    }
    return 1
}

function Get-Boolean {
    param($Value)
    if ($null -eq $Value) {
        return $false
    }
    try {
        return [bool]$Value
    } catch {
        return $false
    }
}

function Get-NestedValue {
    param(
        $ObjectValue,
        [string[]]$Path
    )

    $current = $ObjectValue
    foreach ($segment in $Path) {
        if ($null -eq $current) {
            return $null
        }
        $prop = $current.PSObject.Properties[$segment]
        if ($null -eq $prop) {
            return $null
        }
        $current = $prop.Value
    }
    return $current
}

function Parse-UtcDateTime {
    param(
        $TimestampValue,
        [datetime]$DefaultUtc
    )

    if ($null -eq $TimestampValue) {
        return $DefaultUtc
    }

    $text = [string]$TimestampValue
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $DefaultUtc
    }

    $dto = [DateTimeOffset]::MinValue
    if ([DateTimeOffset]::TryParse($text, [ref]$dto)) {
        return $dto.UtcDateTime
    }

    $dt = [datetime]::MinValue
    if ([datetime]::TryParse($text, [ref]$dt)) {
        return $dt.ToUniversalTime()
    }

    return $DefaultUtc
}

function Get-WeekStartUtc {
    param([datetime]$DateTimeUtc)
    $date = $DateTimeUtc.Date
    $daysFromMonday = ([int]$date.DayOfWeek + 6) % 7
    return $date.AddDays(-1 * $daysFromMonday)
}

function Load-JsonOrNull {
    param([string]$PathValue)
    if (-not (Test-Path $PathValue)) {
        return $null
    }
    try {
        return (Get-Content -Raw $PathValue | ConvertFrom-Json -ErrorAction Stop)
    } catch {
        return $null
    }
}

function Clamp-Int {
    param(
        [int]$Value,
        [int]$Minimum,
        [int]$Maximum
    )
    $lower = [Math]::Min($Minimum, $Maximum)
    $upper = [Math]::Max($Minimum, $Maximum)
    return [Math]::Max($lower, [Math]::Min($upper, $Value))
}

function Clamp-Double {
    param(
        [double]$Value,
        [double]$Minimum,
        [double]$Maximum
    )
    $lower = [Math]::Min($Minimum, $Maximum)
    $upper = [Math]::Max($Minimum, $Maximum)
    return [Math]::Max($lower, [Math]::Min($upper, $Value))
}

function Get-Quantile {
    param(
        [double[]]$Values,
        [double]$Quantile
    )

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return $null
    }

    $q = Clamp-Double -Value $Quantile -Minimum 0.0 -Maximum 1.0
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 1) {
        return [double]$sorted[0]
    }

    $position = ($sorted.Count - 1) * $q
    $lowerIndex = [Math]::Floor($position)
    $upperIndex = [Math]::Ceiling($position)
    if ($lowerIndex -eq $upperIndex) {
        return [double]$sorted[$lowerIndex]
    }

    $fraction = $position - $lowerIndex
    $lowerValue = [double]$sorted[$lowerIndex]
    $upperValue = [double]$sorted[$upperIndex]
    return ($lowerValue + (($upperValue - $lowerValue) * $fraction))
}

function Normalize-HistoryRecord {
    param(
        $Record,
        [datetime]$FallbackNowUtc
    )

    $timestampRaw = Get-NestedValue -ObjectValue $Record -Path @("run_timestamp_utc")
    if ($null -eq $timestampRaw) {
        $timestampRaw = Get-NestedValue -ObjectValue $Record -Path @("run_timestamp")
    }
    if ($null -eq $timestampRaw) {
        $timestampRaw = Get-NestedValue -ObjectValue $Record -Path @("generated_at")
    }

    $runUtc = Parse-UtcDateTime -TimestampValue $timestampRaw -DefaultUtc $FallbackNowUtc
    $runDateUtc = $runUtc.ToString("yyyy-MM-dd")
    $weekStartDateUtc = (Get-WeekStartUtc -DateTimeUtc $runUtc).ToString("yyyy-MM-dd")

    $strictPassedRaw = Get-NestedValue -ObjectValue $Record -Path @("checks", "strict_gate_passed")
    if ($null -eq $strictPassedRaw) {
        $strictPassedRaw = Get-NestedValue -ObjectValue $Record -Path @("checks", "strict_passed")
    }

    $warningPresentRaw = Get-NestedValue -ObjectValue $Record -Path @("metrics", "warning_present")
    if ($null -eq $warningPresentRaw) {
        $warningCount = Get-ArrayCount (Get-NestedValue -ObjectValue $Record -Path @("warnings"))
        $warningPresentRaw = ($warningCount -gt 0)
    }

    $operationalErrorCountRaw = Get-NestedValue -ObjectValue $Record -Path @("metrics", "operational_error_count")
    if ($null -eq $operationalErrorCountRaw) {
        $operationalErrorCountRaw = 0
    }
    $parityErrorCountRaw = Get-NestedValue -ObjectValue $Record -Path @("metrics", "parity_error_count")
    if ($null -eq $parityErrorCountRaw) {
        $parityErrorCountRaw = 0
    }

    return [pscustomobject]@{
        run_timestamp_utc = $runUtc
        run_date_utc = $runDateUtc
        week_start_date_utc = $weekStartDateUtc
        strict_gate_passed = (Get-Boolean -Value $strictPassedRaw)
        warning_present = (Get-Boolean -Value $warningPresentRaw)
        operational_error_count = [int]$operationalErrorCountRaw
        parity_error_count = [int]$parityErrorCountRaw
    }
}

function Get-PolicyExecutionBoundary {
    param(
        [string]$PolicyId,
        [string]$Severity,
        [string]$ExecutionPolicy
    )

    $safeAutoCandidatePolicies = @(
        "warning_ratio_high",
        "warnings_present_below_threshold",
        "strict_live_gate_healthy"
    )

    $isSafeAutoCandidate = (
        ($Severity -eq "warning" -or $Severity -eq "info") -and
        ($safeAutoCandidatePolicies -contains $PolicyId)
    )
    $safeAutoEnabled = ($ExecutionPolicy -eq "safe-auto-execute" -and $isSafeAutoCandidate)

    $reason = if ($safeAutoEnabled) {
        "safe_auto_execute_enabled_by_policy"
    } elseif ($isSafeAutoCandidate) {
        "safe_auto_execute_available_but_not_selected"
    } else {
        "report_only_required_for_risk_level"
    }

    return [pscustomobject]@{
        mode = if ($safeAutoEnabled) { "safe-auto-execute" } else { "report-only" }
        auto_execute_eligible = $isSafeAutoCandidate
        auto_execute_enabled = $safeAutoEnabled
        resume_requires_manual_approval = ($Severity -eq "critical")
        resume_approval_scope = if ($Severity -eq "critical") { "strict_live_schedule_resume" } else { "none" }
        reason = $reason
    }
}

function New-ActionPolicy {
    param(
        [string]$Id,
        [string]$Severity,
        [hashtable]$Evidence,
        [string[]]$AutomaticActions,
        [string[]]$ManualActions,
        [string[]]$CommandHints,
        [string]$Escalation
    )

    return [pscustomobject]@{
        id = $Id
        severity = $Severity
        triggered = $true
        evidence = $Evidence
        automatic_actions = $AutomaticActions
        manual_actions = $ManualActions
        command_hints = $CommandHints
        escalation = $Escalation
        execution_boundary = (Get-PolicyExecutionBoundary -PolicyId $Id -Severity $Severity -ExecutionPolicy $ActionExecutionPolicy)
    }
}

$resolvedExecutionParityReportPath = Resolve-RepoPath $ExecutionParityReportPath
$resolvedOperationalReadinessReportPath = Resolve-RepoPath $OperationalReadinessReportPath
$resolvedHistoryPath = Resolve-RepoPath $HistoryPath
$resolvedDailySummaryJsonPath = Resolve-RepoPath $DailySummaryJsonPath
$resolvedWeeklySummaryJsonPath = Resolve-RepoPath $WeeklySummaryJsonPath
$resolvedDailySummaryCsvPath = Resolve-RepoPath $DailySummaryCsvPath
$resolvedWeeklySummaryCsvPath = Resolve-RepoPath $WeeklySummaryCsvPath
$resolvedAlertOutputJsonPath = Resolve-RepoPath $AlertOutputJsonPath
$resolvedThresholdTuningOutputJsonPath = Resolve-RepoPath $ThresholdTuningOutputJsonPath
$resolvedActionResponseOutputJsonPath = Resolve-RepoPath $ActionResponseOutputJsonPath

Ensure-ParentDirectory $resolvedHistoryPath
Ensure-ParentDirectory $resolvedDailySummaryJsonPath
Ensure-ParentDirectory $resolvedWeeklySummaryJsonPath
Ensure-ParentDirectory $resolvedDailySummaryCsvPath
Ensure-ParentDirectory $resolvedWeeklySummaryCsvPath
Ensure-ParentDirectory $resolvedAlertOutputJsonPath
Ensure-ParentDirectory $resolvedThresholdTuningOutputJsonPath
Ensure-ParentDirectory $resolvedActionResponseOutputJsonPath

$operationalReport = Load-JsonOrNull $resolvedOperationalReadinessReportPath
$executionParityReport = Load-JsonOrNull $resolvedExecutionParityReportPath
$previousActionResponseReport = Load-JsonOrNull $resolvedActionResponseOutputJsonPath

$nowUtc = (Get-Date).ToUniversalTime()
$runTimestampSource = Get-NestedValue -ObjectValue $operationalReport -Path @("generated_at")
if ($null -eq $runTimestampSource) {
    $runTimestampSource = Get-NestedValue -ObjectValue $executionParityReport -Path @("generated_at")
}
$runUtc = Parse-UtcDateTime -TimestampValue $runTimestampSource -DefaultUtc $nowUtc
$runDateUtc = $runUtc.ToString("yyyy-MM-dd")
$weekStartDateUtc = (Get-WeekStartUtc -DateTimeUtc $runUtc).ToString("yyyy-MM-dd")

$operationalReportAvailable = ($null -ne $operationalReport)
$executionParityReportAvailable = ($null -ne $executionParityReport)

$operationalErrors = @((Get-NestedValue -ObjectValue $operationalReport -Path @("errors")))
$parityErrors = @((Get-NestedValue -ObjectValue $executionParityReport -Path @("errors")))
$operationalErrorCount = Get-ArrayCount $operationalErrors
$parityErrorCount = Get-ArrayCount $parityErrors

$operationalWarnings = @()
$operationalWarnings += @((Get-NestedValue -ObjectValue $operationalReport -Path @("warnings")))
$operationalWarnings += @((Get-NestedValue -ObjectValue $operationalReport -Path @("recovery", "warnings")))
$operationalWarnings += @((Get-NestedValue -ObjectValue $operationalReport -Path @("replay_reconcile", "warnings")))
$operationalWarnings += @((Get-NestedValue -ObjectValue $operationalReport -Path @("backtest", "warnings")))
$operationalWarningCount = Get-ArrayCount $operationalWarnings

$parityWarnings = @((Get-NestedValue -ObjectValue $executionParityReport -Path @("warnings")))
$parityWarningCount = Get-ArrayCount $parityWarnings
$warningCount = $operationalWarningCount + $parityWarningCount

$recoveryPassed = Get-Boolean (Get-NestedValue -ObjectValue $operationalReport -Path @("checks", "recovery_e2e_passed"))
$replayReconcilePassed = Get-Boolean (Get-NestedValue -ObjectValue $operationalReport -Path @("checks", "replay_reconcile_diff_passed"))
$operationalParityPassed = Get-Boolean (Get-NestedValue -ObjectValue $operationalReport -Path @("checks", "execution_parity_passed"))
$backtestExecuted = Get-NestedValue -ObjectValue $operationalReport -Path @("checks", "backtest_readiness_executed")
$backtestPassed = $true
if ($null -ne $backtestExecuted -and (Get-Boolean -Value $backtestExecuted)) {
    $backtestPassed = Get-Boolean (Get-NestedValue -ObjectValue $operationalReport -Path @("checks", "backtest_readiness_passed"))
}

$operationalChecksPassed = (
    $recoveryPassed -and
    $replayReconcilePassed -and
    $operationalParityPassed -and
    $backtestPassed
)
$operationalPassed = ($operationalReportAvailable -and $operationalErrorCount -eq 0 -and $operationalChecksPassed)

$parityPassed = $false
if ($executionParityReportAvailable) {
    $parityPassed = ($parityErrorCount -eq 0)
} elseif ($operationalReportAvailable) {
    $parityPassed = $operationalParityPassed
}

$strictGatePassed = ($operationalPassed -and $parityPassed)

$recordWarnings = @()
if (-not $operationalReportAvailable) {
    $recordWarnings += "operational_readiness_report_missing"
}
if (-not $executionParityReportAvailable) {
    $recordWarnings += "execution_parity_report_missing"
}
if ($warningCount -gt 0) {
    $recordWarnings += "warning_signals_present"
}

$recordErrors = @()
if (-not $strictGatePassed) {
    $recordErrors += "strict_live_gate_failed"
}
if ($operationalErrorCount -gt 0) {
    $recordErrors += "operational_readiness_errors_present"
}
if ($parityErrorCount -gt 0) {
    $recordErrors += "execution_parity_errors_present"
}

$record = [ordered]@{
    schema_version = 1
    recorded_at = (Get-Date).ToString("o")
    run_timestamp_utc = $runUtc.ToString("o")
    run_date_utc = $runDateUtc
    week_start_date_utc = $weekStartDateUtc
    gate_profile = $GateProfile
    checks = [ordered]@{
        operational_report_available = $operationalReportAvailable
        execution_parity_report_available = $executionParityReportAvailable
        operational_passed = $operationalPassed
        execution_parity_passed = $parityPassed
        strict_gate_passed = $strictGatePassed
    }
    metrics = [ordered]@{
        operational_error_count = $operationalErrorCount
        parity_error_count = $parityErrorCount
        operational_warning_count = $operationalWarningCount
        parity_warning_count = $parityWarningCount
        warning_count = $warningCount
        warning_present = ($warningCount -gt 0)
    }
    source_reports = [ordered]@{
        operational_readiness_report = [ordered]@{
            path = $resolvedOperationalReadinessReportPath
            available = $operationalReportAvailable
        }
        execution_parity_report = [ordered]@{
            path = $resolvedExecutionParityReportPath
            available = $executionParityReportAvailable
        }
    }
    warnings = $recordWarnings
    errors = $recordErrors
    ci = [ordered]@{
        github_run_id = $env:GITHUB_RUN_ID
        github_run_number = $env:GITHUB_RUN_NUMBER
        github_workflow = $env:GITHUB_WORKFLOW
        github_job = $env:GITHUB_JOB
        github_sha = $env:GITHUB_SHA
    }
}

$historyRecords = New-Object "System.Collections.Generic.List[object]"
$historyParseErrorCount = 0
if (Test-Path $resolvedHistoryPath) {
    foreach ($line in Get-Content -Path $resolvedHistoryPath) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        try {
            $historyRecords.Add(($line | ConvertFrom-Json -ErrorAction Stop))
        } catch {
            $historyParseErrorCount++
        }
    }
}

$historyRecords.Add([pscustomobject]$record)
([pscustomobject]$record | ConvertTo-Json -Depth 8 -Compress) | Add-Content -Path $resolvedHistoryPath -Encoding UTF8

$normalizedRecords = New-Object "System.Collections.Generic.List[object]"
foreach ($historyRecord in $historyRecords) {
    $normalizedRecords.Add((Normalize-HistoryRecord -Record $historyRecord -FallbackNowUtc $nowUtc))
}

$dailySummaries = @()
foreach ($group in ($normalizedRecords | Sort-Object run_date_utc, run_timestamp_utc | Group-Object run_date_utc)) {
    $totalRuns = @($group.Group).Count
    $strictPassRuns = @($group.Group | Where-Object { $_.strict_gate_passed }).Count
    $warningRuns = @($group.Group | Where-Object { $_.warning_present }).Count
    $strictFailRuns = $totalRuns - $strictPassRuns
    $warningRatio = if ($totalRuns -gt 0) { [Math]::Round(($warningRuns / [double]$totalRuns), 4) } else { 0.0 }
    $operationalErrorEvents = ($group.Group | Measure-Object -Property operational_error_count -Sum).Sum
    $parityErrorEvents = ($group.Group | Measure-Object -Property parity_error_count -Sum).Sum
    if ($null -eq $operationalErrorEvents) { $operationalErrorEvents = 0 }
    if ($null -eq $parityErrorEvents) { $parityErrorEvents = 0 }

    $dailySummaries += [pscustomobject]@{
        date_utc = $group.Name
        total_runs = $totalRuns
        strict_pass_runs = $strictPassRuns
        strict_fail_runs = $strictFailRuns
        warning_runs = $warningRuns
        warning_ratio = $warningRatio
        operational_error_events = [int]$operationalErrorEvents
        parity_error_events = [int]$parityErrorEvents
    }
}

$weeklySummaries = @()
foreach ($group in ($normalizedRecords | Sort-Object week_start_date_utc, run_timestamp_utc | Group-Object week_start_date_utc)) {
    $totalRuns = @($group.Group).Count
    $strictPassRuns = @($group.Group | Where-Object { $_.strict_gate_passed }).Count
    $warningRuns = @($group.Group | Where-Object { $_.warning_present }).Count
    $strictFailRuns = $totalRuns - $strictPassRuns
    $warningRatio = if ($totalRuns -gt 0) { [Math]::Round(($warningRuns / [double]$totalRuns), 4) } else { 0.0 }
    $operationalErrorEvents = ($group.Group | Measure-Object -Property operational_error_count -Sum).Sum
    $parityErrorEvents = ($group.Group | Measure-Object -Property parity_error_count -Sum).Sum
    if ($null -eq $operationalErrorEvents) { $operationalErrorEvents = 0 }
    if ($null -eq $parityErrorEvents) { $parityErrorEvents = 0 }

    $weeklySummaries += [pscustomobject]@{
        week_start_date_utc = $group.Name
        total_runs = $totalRuns
        strict_pass_runs = $strictPassRuns
        strict_fail_runs = $strictFailRuns
        warning_runs = $warningRuns
        warning_ratio = $warningRatio
        operational_error_events = [int]$operationalErrorEvents
        parity_error_events = [int]$parityErrorEvents
    }
}

if (@($dailySummaries).Count -gt 0) {
    $dailySummaries | Export-Csv -Path $resolvedDailySummaryCsvPath -NoTypeInformation -Encoding UTF8
} else {
    "" | Set-Content -Path $resolvedDailySummaryCsvPath -Encoding UTF8
}
if (@($weeklySummaries).Count -gt 0) {
    $weeklySummaries | Export-Csv -Path $resolvedWeeklySummaryCsvPath -NoTypeInformation -Encoding UTF8
} else {
    "" | Set-Content -Path $resolvedWeeklySummaryCsvPath -Encoding UTF8
}

$tuningLookbackDaysValue = [Math]::Abs($TuningLookbackDays)
$tuningWindowStartUtc = $nowUtc.AddDays(-1 * $tuningLookbackDaysValue)
$tuningRecords = @($normalizedRecords | Where-Object { $_.run_timestamp_utc -ge $tuningWindowStartUtc } | Sort-Object run_timestamp_utc)
$tuningRecordCount = @($tuningRecords).Count

$consecutiveThresholdMin = Clamp-Int -Value $ConsecutiveFailureThresholdMin -Minimum 1 -Maximum 20
$consecutiveThresholdMax = Clamp-Int -Value $ConsecutiveFailureThresholdMax -Minimum $consecutiveThresholdMin -Maximum 20
$consecutiveThresholdBaseline = Clamp-Int -Value $ConsecutiveFailureThreshold -Minimum $consecutiveThresholdMin -Maximum $consecutiveThresholdMax
$warningThresholdBaseline = Clamp-Double -Value $WarningRatioThreshold -Minimum 0.0 -Maximum 1.0
$warningThresholdFloor = Clamp-Double -Value $WarningRatioThresholdMin -Minimum 0.0 -Maximum 1.0
$warningThresholdCeil = Clamp-Double -Value $WarningRatioThresholdMax -Minimum 0.0 -Maximum 1.0

$consecutiveFailureSeries = @()
$streak = 0
foreach ($entry in $tuningRecords) {
    if ($entry.strict_gate_passed) {
        $streak = 0
    } else {
        $streak++
    }
    $consecutiveFailureSeries += $streak
}

$consecutiveCandidateMetrics = @()
$consecutiveThresholdRecommended = $consecutiveThresholdBaseline
$consecutiveThresholdTuningReady = ($tuningRecordCount -ge $TuningMinHistoryRuns -and $tuningRecordCount -gt 0)
$bestDelta = [double]::PositiveInfinity
$bestThreshold = $consecutiveThresholdBaseline

for ($candidate = $consecutiveThresholdMin; $candidate -le $consecutiveThresholdMax; $candidate++) {
    $alertRuns = @($consecutiveFailureSeries | Where-Object { $_ -ge $candidate }).Count
    $alertRate = if ($tuningRecordCount -gt 0) { [Math]::Round(($alertRuns / [double]$tuningRecordCount), 4) } else { 0.0 }
    $delta = [Math]::Abs($alertRate - $ConsecutiveFailureAlertRateTarget)

    $consecutiveCandidateMetrics += [pscustomobject]@{
        threshold = $candidate
        alert_runs = $alertRuns
        alert_rate = $alertRate
        delta_to_target = [Math]::Round($delta, 4)
    }

    if ($delta -lt $bestDelta -or ($delta -eq $bestDelta -and $candidate -lt $bestThreshold)) {
        $bestDelta = $delta
        $bestThreshold = $candidate
    }
}

if ($consecutiveThresholdTuningReady) {
    $consecutiveThresholdRecommended = $bestThreshold
}

$warningDailySamples = @()
$tuningWindowStartDate = $tuningWindowStartUtc.ToString("yyyy-MM-dd")
foreach ($item in ($dailySummaries | Where-Object { $_.date_utc -ge $tuningWindowStartDate })) {
    $warningDailySamples += [double]$item.warning_ratio
}

$warningSampleCount = @($warningDailySamples).Count
$warningThresholdTuningReady = ($warningSampleCount -ge $WarningRatioTuningMinSamples)
$warningThresholdRecommended = $warningThresholdBaseline
$warningQuantileValue = $null
if ($warningThresholdTuningReady) {
    $warningQuantileValue = Get-Quantile -Values $warningDailySamples -Quantile $WarningRatioQuantile
    if ($null -ne $warningQuantileValue) {
        $warningThresholdRecommended = [Math]::Round(
            (Clamp-Double -Value $warningQuantileValue -Minimum $warningThresholdFloor -Maximum $warningThresholdCeil),
            4
        )
    }
}

$feedbackWeeklyLookbackWeeksValue = [Math]::Max(1, [Math]::Abs($FeedbackWeeklyLookbackWeeks))
$feedbackWeeklyMinRunsValue = [Math]::Max(1, [Math]::Abs($FeedbackWeeklyMinRunsPerWeek))
$feedbackWeeklySignalThresholdValue = [Math]::Round(
    (Clamp-Double -Value $FeedbackWeeklySignalDriftThreshold -Minimum 0.0 -Maximum 1.0),
    4
)
$feedbackConsecutiveCap = [Math]::Max(0, [Math]::Abs($FeedbackConsecutiveAdjustmentCap))
$feedbackWarningCap = [Math]::Round(
    (Clamp-Double -Value ([Math]::Abs($FeedbackWarningRatioAdjustmentCap)) -Minimum 0.0 -Maximum 0.5),
    4
)

$feedbackWeeklySignalDiagnostics = @()
$weeklyCandidates = @($weeklySummaries | Sort-Object week_start_date_utc -Descending | Select-Object -First $feedbackWeeklyLookbackWeeksValue)
foreach ($weekItem in $weeklyCandidates) {
    $weekStartDate = [string]$weekItem.week_start_date_utc
    $weekRecords = @($normalizedRecords | Where-Object { $_.week_start_date_utc -eq $weekStartDate })
    $weekTotalRuns = @($weekRecords).Count
    $falsePositiveRuns = @($weekRecords | Where-Object { $_.strict_gate_passed -and $_.warning_present }).Count
    $falseNegativeRuns = @($weekRecords | Where-Object { -not $_.strict_gate_passed -and -not $_.warning_present }).Count
    $falsePositiveRatio = if ($weekTotalRuns -gt 0) { [Math]::Round(($falsePositiveRuns / [double]$weekTotalRuns), 4) } else { 0.0 }
    $falseNegativeRatio = if ($weekTotalRuns -gt 0) { [Math]::Round(($falseNegativeRuns / [double]$weekTotalRuns), 4) } else { 0.0 }

    $eligibleForDriftCheck = ($weekTotalRuns -ge $feedbackWeeklyMinRunsValue)
    $dominantSignal = "insufficient_samples"
    if ($eligibleForDriftCheck) {
        if ($falsePositiveRatio -ge $feedbackWeeklySignalThresholdValue -and $falseNegativeRatio -ge $feedbackWeeklySignalThresholdValue) {
            $dominantSignal = "mixed"
        } elseif ($falsePositiveRatio -ge $feedbackWeeklySignalThresholdValue) {
            $dominantSignal = "reduce_false_positive_risk"
        } elseif ($falseNegativeRatio -ge $feedbackWeeklySignalThresholdValue) {
            $dominantSignal = "reduce_false_negative_risk"
        } else {
            $dominantSignal = "stable"
        }
    }

    $feedbackWeeklySignalDiagnostics += [pscustomobject]@{
        week_start_date_utc = $weekStartDate
        total_runs = $weekTotalRuns
        false_positive_runs = $falsePositiveRuns
        false_negative_runs = $falseNegativeRuns
        false_positive_ratio = $falsePositiveRatio
        false_negative_ratio = $falseNegativeRatio
        signal_threshold = $feedbackWeeklySignalThresholdValue
        eligible_for_drift_check = $eligibleForDriftCheck
        dominant_signal = $dominantSignal
    }
}

$eligibleWeeklySignals = @($feedbackWeeklySignalDiagnostics | Where-Object { $_.eligible_for_drift_check })
$eligibleWeeklySignalCount = @($eligibleWeeklySignals).Count
$nonStableWeeklySignals = @($eligibleWeeklySignals | Where-Object {
    $_.dominant_signal -eq "reduce_false_positive_risk" -or $_.dominant_signal -eq "reduce_false_negative_risk"
})
$distinctNonStableSignals = @($nonStableWeeklySignals | ForEach-Object { [string]$_.dominant_signal } | Select-Object -Unique)
$mixedSignalWeeks = @($eligibleWeeklySignals | Where-Object { $_.dominant_signal -eq "mixed" }).Count

$feedbackWeeklyAccumulatedSignal = "stable"
$feedbackWeeklyFalsePositiveWeeks = @($eligibleWeeklySignals | Where-Object { $_.dominant_signal -eq "reduce_false_positive_risk" }).Count
$feedbackWeeklyFalseNegativeWeeks = @($eligibleWeeklySignals | Where-Object { $_.dominant_signal -eq "reduce_false_negative_risk" }).Count
if ($feedbackWeeklyFalsePositiveWeeks -gt $feedbackWeeklyFalseNegativeWeeks) {
    $feedbackWeeklyAccumulatedSignal = "reduce_false_positive_risk"
} elseif ($feedbackWeeklyFalseNegativeWeeks -gt $feedbackWeeklyFalsePositiveWeeks) {
    $feedbackWeeklyAccumulatedSignal = "reduce_false_negative_risk"
} elseif ($feedbackWeeklyFalseNegativeWeeks -gt 0 -and $feedbackWeeklyFalsePositiveWeeks -gt 0) {
    $feedbackWeeklyAccumulatedSignal = "mixed"
}

$feedbackWeeklyDriftDetected = ($distinctNonStableSignals.Count -gt 1 -or $mixedSignalWeeks -gt 0)
$feedbackWeeklyStabilizationReady = (
    $eligibleWeeklySignalCount -ge 2 -and
    -not $feedbackWeeklyDriftDetected -and
    $feedbackWeeklyAccumulatedSignal -ne "mixed"
)
$feedbackWeeklyStabilizationNote = if ($eligibleWeeklySignalCount -lt 2) {
    "feedback_loop_waiting_for_weekly_samples"
} elseif ($feedbackWeeklyDriftDetected) {
    "feedback_loop_paused_due_to_weekly_drift"
} elseif ($feedbackWeeklyAccumulatedSignal -eq "mixed") {
    "feedback_loop_paused_due_to_mixed_weekly_signals"
} else {
    "feedback_loop_weekly_stable"
}

$feedbackLoopEnabled = [bool]$EnableActionFeedbackLoop
$feedbackStabilizationHistoryMin = [Math]::Max(1, [Math]::Abs($FeedbackStabilizationMinHistoryRuns))
$feedbackStabilizationWarningMin = [Math]::Max(1, [Math]::Abs($FeedbackStabilizationMinWarningSamples))
$feedbackConsecutiveStep = [Math]::Max(0, [Math]::Abs($FeedbackConsecutiveAdjustmentStep))
$feedbackWarningStep = [Math]::Round(
    (Clamp-Double -Value ([Math]::Abs($FeedbackWarningRatioAdjustmentStep)) -Minimum 0.0 -Maximum 0.2),
    4
)

$previousFeedbackAvailable = $false
$previousFeedbackSignal = "none"
$previousFeedbackReason = "feedback_not_available"
if ($feedbackLoopEnabled -and $null -ne $previousActionResponseReport) {
    $signalCandidate = [string](Get-NestedValue -ObjectValue $previousActionResponseReport -Path @("feedback_for_next_tuning", "stabilization_signal"))
    if (-not [string]::IsNullOrWhiteSpace($signalCandidate)) {
        $previousFeedbackAvailable = $true
        $previousFeedbackSignal = $signalCandidate
        $reasonCandidate = [string](Get-NestedValue -ObjectValue $previousActionResponseReport -Path @("feedback_for_next_tuning", "signal_reason"))
        if (-not [string]::IsNullOrWhiteSpace($reasonCandidate)) {
            $previousFeedbackReason = $reasonCandidate
        } else {
            $previousFeedbackReason = "feedback_signal_present_without_reason"
        }
    }
}

$feedbackStabilizationHistoryReady = ($tuningRecordCount -ge $feedbackStabilizationHistoryMin)
$feedbackStabilizationWarningReady = ($warningSampleCount -ge $feedbackStabilizationWarningMin)
$feedbackStabilizationReady = (
    $feedbackStabilizationHistoryReady -and
    $feedbackStabilizationWarningReady -and
    $feedbackWeeklyStabilizationReady
)
$feedbackStabilizationBlockingReason = if (-not $feedbackStabilizationHistoryReady) {
    "feedback_loop_waiting_for_history_samples"
} elseif (-not $feedbackStabilizationWarningReady) {
    "feedback_loop_waiting_for_warning_samples"
} else {
    $feedbackWeeklyStabilizationNote
}
$feedbackConsecutiveAdjustmentRequested = 0
$feedbackWarningAdjustmentRequested = 0.0
$feedbackConsecutiveAdjustmentApplied = 0
$feedbackWarningAdjustmentApplied = 0.0
$feedbackAdjustmentGuardrailApplied = $false
$feedbackAdjustmentApplied = $false
$feedbackAdjustmentNote = if (-not $feedbackLoopEnabled) {
    "feedback_loop_disabled"
} elseif (-not $previousFeedbackAvailable) {
    "feedback_loop_waiting_for_prior_feedback"
} elseif (-not $feedbackStabilizationReady) {
    $feedbackStabilizationBlockingReason
} else {
    "feedback_loop_ready"
}

if ($feedbackLoopEnabled -and $previousFeedbackAvailable -and $feedbackStabilizationReady) {
    switch ($previousFeedbackSignal) {
        "reduce_false_positive_risk" {
            $feedbackConsecutiveAdjustmentRequested = $feedbackConsecutiveStep
            $feedbackWarningAdjustmentRequested = $feedbackWarningStep
        }
        "reduce_false_negative_risk" {
            $feedbackConsecutiveAdjustmentRequested = -1 * $feedbackConsecutiveStep
            $feedbackWarningAdjustmentRequested = -1.0 * $feedbackWarningStep
        }
    }

    $feedbackConsecutiveAdjustmentApplied = Clamp-Int `
        -Value $feedbackConsecutiveAdjustmentRequested `
        -Minimum (-1 * $feedbackConsecutiveCap) `
        -Maximum $feedbackConsecutiveCap
    $feedbackWarningAdjustmentApplied = [Math]::Round(
        (Clamp-Double `
            -Value $feedbackWarningAdjustmentRequested `
            -Minimum (-1.0 * $feedbackWarningCap) `
            -Maximum $feedbackWarningCap),
        4
    )
    $feedbackAdjustmentGuardrailApplied = (
        $feedbackConsecutiveAdjustmentApplied -ne $feedbackConsecutiveAdjustmentRequested -or
        $feedbackWarningAdjustmentApplied -ne $feedbackWarningAdjustmentRequested
    )

    if ($previousFeedbackSignal -eq "reduce_false_positive_risk") {
        $feedbackAdjustmentNote = "feedback_loop_adjusted_for_false_positive_risk"
    } elseif ($previousFeedbackSignal -eq "reduce_false_negative_risk") {
        $feedbackAdjustmentNote = "feedback_loop_adjusted_for_false_negative_risk"
    } else {
        $feedbackAdjustmentNote = "feedback_loop_signal_is_stable"
    }
    if ($feedbackAdjustmentGuardrailApplied) {
        $feedbackAdjustmentNote = "$feedbackAdjustmentNote`_guardrail_capped"
    }

    $consecutiveThresholdRecommended = Clamp-Int `
        -Value ($consecutiveThresholdRecommended + $feedbackConsecutiveAdjustmentApplied) `
        -Minimum $consecutiveThresholdMin `
        -Maximum $consecutiveThresholdMax
    $warningThresholdRecommended = [Math]::Round(
        (Clamp-Double `
            -Value ($warningThresholdRecommended + $feedbackWarningAdjustmentApplied) `
            -Minimum $warningThresholdFloor `
            -Maximum $warningThresholdCeil),
        4
    )
    $feedbackAdjustmentApplied = ($feedbackConsecutiveAdjustmentApplied -ne 0 -or $feedbackWarningAdjustmentApplied -ne 0.0)
}

$activeConsecutiveFailureThreshold = $consecutiveThresholdBaseline
$activeWarningRatioThreshold = $warningThresholdBaseline
$tunedApplied = $false
if ($ApplyTunedThresholds.IsPresent -and $consecutiveThresholdTuningReady -and $warningThresholdTuningReady) {
    $activeConsecutiveFailureThreshold = $consecutiveThresholdRecommended
    $activeWarningRatioThreshold = $warningThresholdRecommended
    $tunedApplied = $true
}

$thresholdTuningOutput = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    history_path = $resolvedHistoryPath
    tuning_window = [ordered]@{
        lookback_days = $tuningLookbackDaysValue
        start_utc = $tuningWindowStartUtc.ToString("o")
        end_utc = $nowUtc.ToString("o")
        records_considered = $tuningRecordCount
        total_history_records = $normalizedRecords.Count
    }
    baseline_thresholds = [ordered]@{
        consecutive_failure_threshold = $consecutiveThresholdBaseline
        warning_ratio_threshold = $warningThresholdBaseline
        warning_ratio_lookback_days = [Math]::Abs($WarningRatioLookbackDays)
        warning_ratio_min_samples = $WarningRatioMinSamples
    }
    recommended_thresholds = [ordered]@{
        consecutive_failure_threshold = $consecutiveThresholdRecommended
        warning_ratio_threshold = $warningThresholdRecommended
    }
    tuning_readiness = [ordered]@{
        consecutive_failure_tuning_ready = $consecutiveThresholdTuningReady
        warning_ratio_tuning_ready = $warningThresholdTuningReady
        tuning_min_history_runs = $TuningMinHistoryRuns
        warning_ratio_tuning_min_samples = $WarningRatioTuningMinSamples
        feedback_loop_enabled = $feedbackLoopEnabled
        feedback_stabilization_ready = $feedbackStabilizationReady
        feedback_stabilization_blocking_reason = $feedbackStabilizationBlockingReason
        feedback_stabilization_min_history_runs = $feedbackStabilizationHistoryMin
        feedback_stabilization_min_warning_samples = $feedbackStabilizationWarningMin
        feedback_weekly_stabilization_ready = $feedbackWeeklyStabilizationReady
        feedback_weekly_lookback_weeks = $feedbackWeeklyLookbackWeeksValue
        feedback_weekly_min_runs_per_week = $feedbackWeeklyMinRunsValue
        feedback_weekly_signal_drift_threshold = $feedbackWeeklySignalThresholdValue
        previous_feedback_available = $previousFeedbackAvailable
    }
    applied_thresholds = [ordered]@{
        apply_tuned_thresholds_requested = [bool]$ApplyTunedThresholds
        tuned_thresholds_applied = $tunedApplied
        consecutive_failure_threshold = $activeConsecutiveFailureThreshold
        warning_ratio_threshold = $activeWarningRatioThreshold
    }
    feedback_loop = [ordered]@{
        previous_feedback = [ordered]@{
            available = $previousFeedbackAvailable
            stabilization_signal = $previousFeedbackSignal
            signal_reason = $previousFeedbackReason
        }
        adjustment = [ordered]@{
            applied = $feedbackAdjustmentApplied
            guardrail_applied = $feedbackAdjustmentGuardrailApplied
            consecutive_failure_threshold_delta_requested = $feedbackConsecutiveAdjustmentRequested
            consecutive_failure_threshold_delta = $feedbackConsecutiveAdjustmentApplied
            warning_ratio_threshold_delta_requested = $feedbackWarningAdjustmentRequested
            warning_ratio_threshold_delta = $feedbackWarningAdjustmentApplied
            note = $feedbackAdjustmentNote
        }
        weekly_signal = [ordered]@{
            lookback_weeks = $feedbackWeeklyLookbackWeeksValue
            min_runs_per_week = $feedbackWeeklyMinRunsValue
            drift_threshold = $feedbackWeeklySignalThresholdValue
            eligible_week_count = $eligibleWeeklySignalCount
            accumulated_signal = $feedbackWeeklyAccumulatedSignal
            drift_detected = $feedbackWeeklyDriftDetected
            stabilization_ready = $feedbackWeeklyStabilizationReady
            stabilization_note = $feedbackWeeklyStabilizationNote
            weeks = $feedbackWeeklySignalDiagnostics
        }
        guardrails = [ordered]@{
            threshold_bounds = [ordered]@{
                consecutive_failure_threshold_min = $consecutiveThresholdMin
                consecutive_failure_threshold_max = $consecutiveThresholdMax
                warning_ratio_threshold_min = $warningThresholdFloor
                warning_ratio_threshold_max = $warningThresholdCeil
            }
            feedback_adjustment_caps = [ordered]@{
                consecutive_failure_threshold_delta_cap = $feedbackConsecutiveCap
                warning_ratio_threshold_delta_cap = $feedbackWarningCap
            }
        }
    }
    diagnostics = [ordered]@{
        consecutive_failure_alert_rate_target = $ConsecutiveFailureAlertRateTarget
        consecutive_failure_candidate_metrics = $consecutiveCandidateMetrics
        warning_ratio_quantile = $WarningRatioQuantile
        warning_ratio_quantile_value = $warningQuantileValue
        warning_ratio_sample_count = $warningSampleCount
        warning_ratio_samples = $warningDailySamples
    }
}
($thresholdTuningOutput | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedThresholdTuningOutputJsonPath -Encoding UTF8

$dailySummaryOutput = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    history_path = $resolvedHistoryPath
    history_record_count = $normalizedRecords.Count
    history_parse_errors_ignored = $historyParseErrorCount
    latest_record = [pscustomobject]$record
    summaries = $dailySummaries
}
($dailySummaryOutput | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedDailySummaryJsonPath -Encoding UTF8

$weeklySummaryOutput = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    history_path = $resolvedHistoryPath
    history_record_count = $normalizedRecords.Count
    history_parse_errors_ignored = $historyParseErrorCount
    latest_record = [pscustomobject]$record
    summaries = $weeklySummaries
}
($weeklySummaryOutput | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedWeeklySummaryJsonPath -Encoding UTF8

$sortedByLatest = @($normalizedRecords | Sort-Object run_timestamp_utc -Descending)
$consecutiveStrictFailureCount = 0
foreach ($entry in $sortedByLatest) {
    if ($entry.strict_gate_passed) {
        break
    }
    $consecutiveStrictFailureCount++
}

$lookbackDaysValue = [Math]::Abs($WarningRatioLookbackDays)
$lookbackStartUtc = $nowUtc.AddDays(-1 * $lookbackDaysValue)
$lookbackRecords = @($normalizedRecords | Where-Object { $_.run_timestamp_utc -ge $lookbackStartUtc })
$lookbackTotalRuns = @($lookbackRecords).Count
$lookbackWarningRuns = @($lookbackRecords | Where-Object { $_.warning_present }).Count
$lookbackWarningRatio = if ($lookbackTotalRuns -gt 0) {
    [Math]::Round(($lookbackWarningRuns / [double]$lookbackTotalRuns), 4)
} else {
    0.0
}

$consecutiveFailureTriggered = ($consecutiveStrictFailureCount -ge $activeConsecutiveFailureThreshold)
$warningRatioTriggered = (
    $lookbackTotalRuns -ge $WarningRatioMinSamples -and
    $lookbackWarningRatio -gt $activeWarningRatioThreshold
)

$alerts = @(
    [pscustomobject]@{
        id = "consecutive_strict_failures"
        severity = if ($consecutiveFailureTriggered) { "critical" } else { "info" }
        triggered = $consecutiveFailureTriggered
        value = $consecutiveStrictFailureCount
        threshold = $activeConsecutiveFailureThreshold
        baseline_threshold = $consecutiveThresholdBaseline
        recommended_threshold = $consecutiveThresholdRecommended
        message = "Consecutive strict gate failures reached threshold."
    },
    [pscustomobject]@{
        id = "warning_ratio_high"
        severity = if ($warningRatioTriggered) { "warning" } else { "info" }
        triggered = $warningRatioTriggered
        value = $lookbackWarningRatio
        threshold = $activeWarningRatioThreshold
        baseline_threshold = $warningThresholdBaseline
        recommended_threshold = $warningThresholdRecommended
        message = "Warning ratio in lookback window exceeded threshold."
    }
)

$overallStatus = "ok"
if ($consecutiveFailureTriggered) {
    $overallStatus = "critical"
} elseif ($warningRatioTriggered) {
    $overallStatus = "warning"
}
$shouldFailGate = ($overallStatus -eq "critical")

$alertOutput = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    thresholds = [ordered]@{
        consecutive_failure_threshold = $activeConsecutiveFailureThreshold
        warning_ratio_threshold = $activeWarningRatioThreshold
        warning_ratio_lookback_days = $lookbackDaysValue
        warning_ratio_min_samples = $WarningRatioMinSamples
        tuned_thresholds_applied = $tunedApplied
        baseline = [ordered]@{
            consecutive_failure_threshold = $consecutiveThresholdBaseline
            warning_ratio_threshold = $warningThresholdBaseline
        }
        recommended = [ordered]@{
            consecutive_failure_threshold = $consecutiveThresholdRecommended
            warning_ratio_threshold = $warningThresholdRecommended
        }
    }
    current = [ordered]@{
        latest_run_timestamp_utc = $runUtc.ToString("o")
        latest_run_strict_gate_passed = $strictGatePassed
        consecutive_strict_failures = $consecutiveStrictFailureCount
        lookback_start_utc = $lookbackStartUtc.ToString("o")
        lookback_total_runs = $lookbackTotalRuns
        lookback_warning_runs = $lookbackWarningRuns
        lookback_warning_ratio = $lookbackWarningRatio
    }
    alerts = $alerts
    overall_status = $overallStatus
    should_fail_gate = $shouldFailGate
}
($alertOutput | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedAlertOutputJsonPath -Encoding UTF8

$actionPolicies = @()

if (-not $operationalReportAvailable) {
    $actionPolicies += New-ActionPolicy `
        -Id "operational_report_missing" `
        -Severity "critical" `
        -Evidence @{ report = $resolvedOperationalReadinessReportPath } `
        -AutomaticActions @("Flag strict live gate run as degraded and publish missing-report alert.") `
        -ManualActions @("Re-run strict operational gate wrapper and inspect report generation path.") `
        -CommandHints @(
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_ci_operational_gate.ps1 -IncludeBacktest -RunLiveProbe -StrictExecutionParity"
        ) `
        -Escalation "ops_oncall"
}

if (-not $executionParityReportAvailable) {
    $actionPolicies += New-ActionPolicy `
        -Id "execution_parity_report_missing" `
        -Severity "critical" `
        -Evidence @{ report = $resolvedExecutionParityReportPath } `
        -AutomaticActions @("Emit parity-missing alert and keep strict status unresolved.") `
        -ManualActions @("Regenerate parity artifacts by running strict operational gate wrapper.") `
        -CommandHints @(
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_ci_operational_gate.ps1 -IncludeBacktest -RunLiveProbe -StrictExecutionParity"
        ) `
        -Escalation "ops_oncall"
}

if ($parityErrorCount -gt 0) {
    $actionPolicies += New-ActionPolicy `
        -Id "execution_parity_errors_present" `
        -Severity "critical" `
        -Evidence @{ parity_error_count = $parityErrorCount; parity_errors = $parityErrors } `
        -AutomaticActions @("Mark run as strict-failed and attach parity error evidence.") `
        -ManualActions @("Inspect schema mismatch or missing update stream root cause and rerun strict gate.") `
        -CommandHints @(
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_execution_parity.ps1 -Strict",
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_ci_operational_gate.ps1 -IncludeBacktest -RunLiveProbe -StrictExecutionParity"
        ) `
        -Escalation "ops_oncall"
}

if ($operationalErrorCount -gt 0) {
    $actionPolicies += New-ActionPolicy `
        -Id "operational_readiness_errors_present" `
        -Severity "critical" `
        -Evidence @{ operational_error_count = $operationalErrorCount; operational_errors = $operationalErrors } `
        -AutomaticActions @("Mark run as strict-failed and publish operational error summary.") `
        -ManualActions @("Run recovery/reconcile strict validators and investigate failing component.") `
        -CommandHints @(
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_recovery_e2e.ps1 -StrictLogCheck -OutputJson build/Release/logs/recovery_e2e_report_strict.json -StateValidationJson build/Release/logs/recovery_state_validation_strict.json",
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_replay_reconcile_diff.ps1 -Strict"
        ) `
        -Escalation "ops_oncall"
}

if ($consecutiveFailureTriggered) {
    $actionPolicies += New-ActionPolicy `
        -Id "consecutive_strict_failures" `
        -Severity "critical" `
        -Evidence @{
            consecutive_failures = $consecutiveStrictFailureCount
            threshold = $activeConsecutiveFailureThreshold
            tuned_thresholds_applied = $tunedApplied
        } `
        -AutomaticActions @(
            "Raise sustained-failure alert for strict live gate history trend.",
            "Set action response status to manual intervention required."
        ) `
        -ManualActions @(
            "Freeze strict live schedule until root cause triage is complete.",
            "Review key health (permissions, allowlist, rotation status) before re-enable."
        ) `
        -CommandHints @(
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_ci_operational_gate.ps1 -IncludeBacktest -RunLiveProbe -StrictExecutionParity",
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/generate_strict_live_gate_trend_alert.ps1 -GateProfile strict_live -ApplyTunedThresholds -ActionExecutionPolicy safe-auto-execute -EnableActionFeedbackLoop"
        ) `
        -Escalation "ops_lead"
}

if ($warningRatioTriggered) {
    $actionPolicies += New-ActionPolicy `
        -Id "warning_ratio_high" `
        -Severity "warning" `
        -Evidence @{
            warning_ratio = $lookbackWarningRatio
            threshold = $activeWarningRatioThreshold
            lookback_total_runs = $lookbackTotalRuns
        } `
        -AutomaticActions @("Publish warning-ratio trend alert in action response report.") `
        -ManualActions @("Review recurring warning categories and adjust threshold profile if needed.") `
        -CommandHints @(
            "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/generate_strict_live_gate_trend_alert.ps1 -GateProfile strict_live -ApplyTunedThresholds -ActionExecutionPolicy safe-auto-execute -EnableActionFeedbackLoop"
        ) `
        -Escalation "ops_maintainer"
}

if ($warningCount -gt 0 -and -not $warningRatioTriggered) {
    $actionPolicies += New-ActionPolicy `
        -Id "warnings_present_below_threshold" `
        -Severity "info" `
        -Evidence @{
            warning_count = $warningCount
            warning_categories = @($operationalWarnings + $parityWarnings)
        } `
        -AutomaticActions @("Attach warning list to action response report for trend tracking.") `
        -ManualActions @("Monitor warning recurrence; no immediate gate stop required.") `
        -CommandHints @() `
        -Escalation "none"
}

if (@($actionPolicies).Count -eq 0) {
    $actionPolicies += New-ActionPolicy `
        -Id "strict_live_gate_healthy" `
        -Severity "info" `
        -Evidence @{
            strict_gate_passed = $strictGatePassed
            warning_count = $warningCount
        } `
        -AutomaticActions @("Publish healthy status in action response report.") `
        -ManualActions @("No manual remediation required.") `
        -CommandHints @() `
        -Escalation "none"
}

$requiresManualIntervention = @($actionPolicies | Where-Object { $_.severity -eq "critical" }).Count -gt 0
$actionOverallStatus = if ($requiresManualIntervention) {
    "manual_intervention_required"
} elseif ($warningRatioTriggered) {
    "monitor"
} else {
    "ok"
}

$boundaryReportOnlyCount = @($actionPolicies | Where-Object { $_.execution_boundary.mode -eq "report-only" }).Count
$boundarySafeAutoCount = @($actionPolicies | Where-Object { $_.execution_boundary.mode -eq "safe-auto-execute" }).Count
$boundaryManualApprovalCount = @($actionPolicies | Where-Object { $_.execution_boundary.resume_requires_manual_approval }).Count
$safeAutoEnabledPolicyIds = @($actionPolicies | Where-Object { $_.execution_boundary.auto_execute_enabled } | ForEach-Object { [string]$_.id })

$falsePositiveIndicator = (
    $warningRatioTriggered -and
    $strictGatePassed -and
    -not $requiresManualIntervention
)
$falseNegativeIndicator = (
    -not $strictGatePassed -and
    -not $warningRatioTriggered -and
    -not $consecutiveFailureTriggered
)

$nextFeedbackSignal = "stable"
$nextFeedbackSignalReason = "no_stabilization_adjustment_needed"
$nextFeedbackSignalSource = "run_level_indicator"
if ($feedbackWeeklyDriftDetected) {
    $nextFeedbackSignal = "stable"
    $nextFeedbackSignalReason = "weekly_drift_detected_feedback_paused"
    $nextFeedbackSignalSource = "weekly_drift_guardrail"
} elseif ($feedbackWeeklyStabilizationReady -and $feedbackWeeklyAccumulatedSignal -eq "reduce_false_positive_risk") {
    $nextFeedbackSignal = "reduce_false_positive_risk"
    $nextFeedbackSignalReason = "weekly_accumulated_false_positive_signal"
    $nextFeedbackSignalSource = "weekly_accumulation"
} elseif ($feedbackWeeklyStabilizationReady -and $feedbackWeeklyAccumulatedSignal -eq "reduce_false_negative_risk") {
    $nextFeedbackSignal = "reduce_false_negative_risk"
    $nextFeedbackSignalReason = "weekly_accumulated_false_negative_signal"
    $nextFeedbackSignalSource = "weekly_accumulation"
} elseif ($feedbackWeeklyStabilizationReady) {
    $nextFeedbackSignal = "stable"
    $nextFeedbackSignalReason = "weekly_accumulated_signal_stable"
    $nextFeedbackSignalSource = "weekly_accumulation"
} elseif ($falsePositiveIndicator) {
    $nextFeedbackSignal = "reduce_false_positive_risk"
    $nextFeedbackSignalReason = "warning_alert_triggered_without_strict_failure"
} elseif ($falseNegativeIndicator) {
    $nextFeedbackSignal = "reduce_false_negative_risk"
    $nextFeedbackSignalReason = "strict_failure_without_warning_or_streak_alert"
}

$recommendedCommands = New-Object "System.Collections.Generic.List[string]"
foreach ($policy in $actionPolicies) {
    foreach ($commandHint in @($policy.command_hints)) {
        if ([string]::IsNullOrWhiteSpace([string]$commandHint)) {
            continue
        }
        if (-not $recommendedCommands.Contains([string]$commandHint)) {
            $recommendedCommands.Add([string]$commandHint)
        }
    }
}

$actionResponseOutput = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    gate_profile = $GateProfile
    action_execution_policy = $ActionExecutionPolicy
    overall_status = $actionOverallStatus
    requires_manual_intervention = $requiresManualIntervention
    latest_run = [ordered]@{
        run_timestamp_utc = $runUtc.ToString("o")
        strict_gate_passed = $strictGatePassed
        operational_passed = $operationalPassed
        execution_parity_passed = $parityPassed
    }
    threshold_context = [ordered]@{
        consecutive_failure_threshold = $activeConsecutiveFailureThreshold
        warning_ratio_threshold = $activeWarningRatioThreshold
        tuned_thresholds_applied = $tunedApplied
        threshold_tuning_report = $resolvedThresholdTuningOutputJsonPath
    }
    policy_boundary_summary = [ordered]@{
        report_only_policy_count = $boundaryReportOnlyCount
        safe_auto_execute_policy_count = $boundarySafeAutoCount
        manual_approval_required_policy_count = $boundaryManualApprovalCount
        safe_auto_execute_policy_ids = $safeAutoEnabledPolicyIds
    }
    manual_approval = [ordered]@{
        approval_required_for_resume = $requiresManualIntervention
        approval_scope = if ($requiresManualIntervention) { "strict_live_schedule_resume" } else { "none" }
        approval_reason = if ($requiresManualIntervention) { "critical_policy_detected" } else { "no_critical_policy" }
        github_environment = [ordered]@{
            required = $requiresManualIntervention
            name = if ($requiresManualIntervention) { "strict-live-resume" } else { "none" }
            required_reviewers_expected = $requiresManualIntervention
        }
        required_evidence_paths = @(
            $resolvedActionResponseOutputJsonPath,
            $resolvedAlertOutputJsonPath,
            $resolvedThresholdTuningOutputJsonPath,
            $resolvedOperationalReadinessReportPath,
            $resolvedExecutionParityReportPath
        )
        resume_checklist = @(
            "Pause strict live schedule while critical policy is open.",
            "Execute recommended_commands and confirm strict gate pass on rerun.",
            "Record approval decision and approver in ops log.",
            "Resume strict live schedule only after approval record is completed."
        )
    }
    feedback_for_next_tuning = [ordered]@{
        schema_version = 1
        source_run_timestamp_utc = $runUtc.ToString("o")
        stabilization_signal = $nextFeedbackSignal
        signal_reason = $nextFeedbackSignalReason
        signal_source = $nextFeedbackSignalSource
        false_positive_indicator = $falsePositiveIndicator
        false_negative_indicator = $falseNegativeIndicator
        stabilization_criteria = [ordered]@{
            min_history_runs = [Math]::Max(1, [Math]::Abs($FeedbackStabilizationMinHistoryRuns))
            min_warning_samples = [Math]::Max(1, [Math]::Abs($FeedbackStabilizationMinWarningSamples))
            weekly_lookback_weeks = $feedbackWeeklyLookbackWeeksValue
            weekly_min_runs_per_week = $feedbackWeeklyMinRunsValue
            weekly_signal_drift_threshold = $feedbackWeeklySignalThresholdValue
        }
        weekly_signal_context = [ordered]@{
            eligible_week_count = $eligibleWeeklySignalCount
            accumulated_signal = $feedbackWeeklyAccumulatedSignal
            drift_detected = $feedbackWeeklyDriftDetected
            stabilization_ready = $feedbackWeeklyStabilizationReady
            stabilization_note = $feedbackWeeklyStabilizationNote
            weeks = $feedbackWeeklySignalDiagnostics
        }
        adjustment_rules = [ordered]@{
            reduce_false_positive_risk = [ordered]@{
                consecutive_failure_threshold_delta = [Math]::Max(0, [Math]::Abs($FeedbackConsecutiveAdjustmentStep))
                warning_ratio_threshold_delta = [Math]::Round((Clamp-Double -Value ([Math]::Abs($FeedbackWarningRatioAdjustmentStep)) -Minimum 0.0 -Maximum 0.2), 4)
            }
            reduce_false_negative_risk = [ordered]@{
                consecutive_failure_threshold_delta = -1 * [Math]::Max(0, [Math]::Abs($FeedbackConsecutiveAdjustmentStep))
                warning_ratio_threshold_delta = -1.0 * [Math]::Round((Clamp-Double -Value ([Math]::Abs($FeedbackWarningRatioAdjustmentStep)) -Minimum 0.0 -Maximum 0.2), 4)
            }
            stable = [ordered]@{
                consecutive_failure_threshold_delta = 0
                warning_ratio_threshold_delta = 0.0
            }
        }
        guardrails = [ordered]@{
            threshold_bounds = [ordered]@{
                consecutive_failure_threshold_min = $consecutiveThresholdMin
                consecutive_failure_threshold_max = $consecutiveThresholdMax
                warning_ratio_threshold_min = $warningThresholdFloor
                warning_ratio_threshold_max = $warningThresholdCeil
            }
            feedback_adjustment_caps = [ordered]@{
                consecutive_failure_threshold_delta_cap = $feedbackConsecutiveCap
                warning_ratio_threshold_delta_cap = $feedbackWarningCap
            }
        }
    }
    policies = $actionPolicies
    recommended_commands = @($recommendedCommands)
}
($actionResponseOutput | ConvertTo-Json -Depth 10) | Set-Content -Path $resolvedActionResponseOutputJsonPath -Encoding UTF8

if ($FailOnCriticalAlert.IsPresent -and $shouldFailGate) {
    Write-Host "[StrictLiveTrendAlert] FAILED (critical alert) - see $resolvedAlertOutputJsonPath"
    exit 1
}

Write-Host "[StrictLiveTrendAlert] PASSED - trend, tuning, and action reports generated under build/Release/logs"
exit 0
