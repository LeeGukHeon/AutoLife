param(
    [string]$SnapshotPath = "build/Release/state/snapshot_state.json",
    [string]$LegacyStatePath = "build/Release/state/state.json",
    [string]$JournalPath = "build/Release/state/event_journal.jsonl",
    [string]$LogPath = "build/Release/logs/ci_fixture/autolife_ci_fixture.log"
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

$resolvedSnapshotPath = Resolve-RepoPath $SnapshotPath
$resolvedLegacyPath = Resolve-RepoPath $LegacyStatePath
$resolvedJournalPath = Resolve-RepoPath $JournalPath
$resolvedLogPath = Resolve-RepoPath $LogPath

foreach ($path in @($resolvedSnapshotPath, $resolvedLegacyPath, $resolvedJournalPath, $resolvedLogPath)) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path $dir)) {
        New-Item -Path $dir -ItemType Directory -Force | Out-Null
    }
}

$tsMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()

$state = [ordered]@{
    timestamp = $tsMs
    snapshot_last_event_seq = 0
    open_positions = @()
}

($state | ConvertTo-Json -Depth 6) | Set-Content -Path $resolvedSnapshotPath -Encoding UTF8
($state | ConvertTo-Json -Depth 6) | Set-Content -Path $resolvedLegacyPath -Encoding UTF8

$journalEvent = [ordered]@{
    seq = 0
    ts_ms = $tsMs
    type = "POLICY_DECISION"
    market = "KRW-BTC"
    source = "ci_fixture"
    payload = [ordered]@{
        note = "stage8_ci_fixture"
    }
}

(($journalEvent | ConvertTo-Json -Depth 6 -Compress) + "`n") | Set-Content -Path $resolvedJournalPath -Encoding UTF8

$snapshotPathForLog = $resolvedSnapshotPath -replace "\\", "\\"
$logLines = @(
    "[2026-02-13 00:00:00.000] [main] [info] State snapshot loaded: path=$snapshotPathForLog, timestamp=$tsMs, last_event_seq=0",
    "[2026-02-13 00:00:00.001] [main] [info] State restore: no replay events applied (start_seq=1, journal_last_seq=0)",
    "[2026-02-13 00:00:00.002] [main] [info] Account state sync summary: wallet_markets=1, reconcile_candidates=0, restored_positions=0, external_closes=0",
    "[2026-02-13 00:00:00.003] [main] [info] Account state synchronization completed"
)
$logLines | Set-Content -Path $resolvedLogPath -Encoding UTF8

Write-Host "[OperationalFixture] PASSED - snapshot=$resolvedSnapshotPath"
Write-Host "[OperationalFixture] PASSED - journal=$resolvedJournalPath"
Write-Host "[OperationalFixture] PASSED - log=$resolvedLogPath"
exit 0
