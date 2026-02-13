param(
    [string]$SnapshotPath = "build/Release/state/snapshot_state.json",
    [string]$LegacyStatePath = "build/Release/state/state.json",
    [string]$JournalPath = "build/Release/state/event_journal.jsonl",
    [string]$OutputJson = "build/Release/logs/recovery_state_validation.json"
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

function Parse-JsonLine {
    param([string]$Line)
    try {
        return ($Line | ConvertFrom-Json)
    } catch {
        return $null
    }
}

$resolvedSnapshotPath = Resolve-RepoPath $SnapshotPath
$resolvedLegacyPath = Resolve-RepoPath $LegacyStatePath
$resolvedJournalPath = Resolve-RepoPath $JournalPath
$resolvedOutputPath = Resolve-RepoPath $OutputJson

$stateSource = $null
if (Test-Path $resolvedSnapshotPath) {
    $stateSource = $resolvedSnapshotPath
} elseif (Test-Path $resolvedLegacyPath) {
    $stateSource = $resolvedLegacyPath
}

$result = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    snapshot_path = $resolvedSnapshotPath
    legacy_state_path = $resolvedLegacyPath
    journal_path = $resolvedJournalPath
    state_source = $stateSource
    checks = [ordered]@{
        state_file_exists = $false
        journal_exists = $false
        state_parsed = $false
        seq_increasing = $false
        snapshot_watermark_valid = $false
    }
    metrics = [ordered]@{
        snapshot_last_event_seq = 0
        snapshot_timestamp = 0
        journal_event_count = 0
        journal_parse_errors = 0
        journal_last_seq = 0
        replayable_event_count = 0
        predicted_open_positions_after_replay = 0
    }
    errors = @()
}

if ($null -eq $stateSource) {
    $result.errors += "state_file_missing"
} else {
    $result.checks.state_file_exists = $true
}

if (-not (Test-Path $resolvedJournalPath)) {
    $result.errors += "journal_missing"
} else {
    $result.checks.journal_exists = $true
}

$stateJson = $null
if ($result.checks.state_file_exists) {
    try {
        $stateJson = Get-Content $stateSource -Raw | ConvertFrom-Json
        $result.checks.state_parsed = $true
    } catch {
        $result.errors += "state_parse_error"
    }
}

$events = @()
if ($result.checks.journal_exists) {
    $lines = Get-Content $resolvedJournalPath
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $parsed = Parse-JsonLine $line
        if ($null -eq $parsed) {
            $result.metrics.journal_parse_errors++
            continue
        }
        $events += $parsed
    }

    $result.metrics.journal_event_count = @($events).Count
    if (@($events).Count -gt 0) {
        $events = @($events | Sort-Object -Property seq)
        $result.metrics.journal_last_seq = [int64]$events[-1].seq

        $increasing = $true
        $previous = -1
        foreach ($e in $events) {
            $seq = [int64]$e.seq
            if ($seq -le $previous) {
                $increasing = $false
                break
            }
            $previous = $seq
        }
        $result.checks.seq_increasing = $increasing
        if (-not $increasing) {
            $result.errors += "journal_seq_not_increasing"
        }
    } else {
        $result.checks.seq_increasing = $true
    }
}

$snapshotLastSeq = 0
$snapshotTimestamp = 0
if ($result.checks.state_parsed) {
    if ($stateJson.PSObject.Properties.Name -contains "snapshot_last_event_seq") {
        $snapshotLastSeq = [int64]$stateJson.snapshot_last_event_seq
    }
    if ($stateJson.PSObject.Properties.Name -contains "timestamp") {
        $snapshotTimestamp = [int64]$stateJson.timestamp
    }
}

$result.metrics.snapshot_last_event_seq = $snapshotLastSeq
$result.metrics.snapshot_timestamp = $snapshotTimestamp

if ($result.checks.journal_exists) {
    if ($snapshotLastSeq -le $result.metrics.journal_last_seq) {
        $result.checks.snapshot_watermark_valid = $true
    } else {
        $result.errors += "snapshot_watermark_exceeds_journal_last_seq"
    }
}

$replayableEvents = @()
if (@($events).Count -gt 0) {
    $replayableEvents = @($events | Where-Object { [int64]$_.seq -gt $snapshotLastSeq })
    $result.metrics.replayable_event_count = $replayableEvents.Count
}

$openMap = @{}
if ($result.checks.state_parsed -and ($stateJson.PSObject.Properties.Name -contains "open_positions")) {
    foreach ($p in $stateJson.open_positions) {
        $market = [string]$p.market
        if ([string]::IsNullOrWhiteSpace($market)) {
            continue
        }
        $openMap[$market] = [ordered]@{
            market = $market
            entry_price = [double]$p.entry_price
            quantity = [double]$p.quantity
        }
    }
}

foreach ($e in $replayableEvents) {
    $market = [string]$e.market
    if ([string]::IsNullOrWhiteSpace($market)) {
        continue
    }

    $payload = $e.payload
    switch ([string]$e.type) {
        "POSITION_OPENED" {
            $openMap[$market] = [ordered]@{
                market = $market
                entry_price = [double]$payload.entry_price
                quantity = [double]$payload.quantity
            }
        }
        "FILL_APPLIED" {
            $side = ""
            if ($payload.PSObject.Properties.Name -contains "side") {
                $side = ([string]$payload.side).ToLowerInvariant()
            }
            if (($side -eq "buy") -and (-not $openMap.ContainsKey($market))) {
                $openMap[$market] = [ordered]@{
                    market = $market
                    entry_price = [double]$payload.avg_price
                    quantity = [double]$payload.filled_volume
                }
            }
        }
        "POSITION_REDUCED" {
            if ($openMap.ContainsKey($market)) {
                $qty = [double]$openMap[$market].quantity
                $reduced = [double]$payload.quantity
                if ($reduced -gt 0) {
                    $qty = [Math]::Max(0.0, $qty - $reduced)
                }
                if ($qty -le 0.0) {
                    $openMap.Remove($market)
                } else {
                    $openMap[$market].quantity = $qty
                }
            }
        }
        "POSITION_CLOSED" {
            if ($openMap.ContainsKey($market)) {
                $openMap.Remove($market)
            }
        }
    }
}

$result.metrics.predicted_open_positions_after_replay = $openMap.Count

$outputDir = Split-Path -Parent $resolvedOutputPath
if (-not (Test-Path $outputDir)) {
    New-Item -Path $outputDir -ItemType Directory -Force | Out-Null
}

($result | ConvertTo-Json -Depth 8) | Set-Content -Path $resolvedOutputPath -Encoding UTF8

$hardFailures = @(
    -not $result.checks.state_file_exists,
    -not $result.checks.journal_exists,
    -not $result.checks.state_parsed,
    -not $result.checks.seq_increasing,
    -not $result.checks.snapshot_watermark_valid
) | Where-Object { $_ } | Measure-Object | Select-Object -ExpandProperty Count

if ($hardFailures -gt 0) {
    Write-Host "[RecoveryValidate] FAILED - see $resolvedOutputPath"
    exit 1
}

Write-Host "[RecoveryValidate] PASSED - see $resolvedOutputPath"
exit 0
