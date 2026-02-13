param(
    [string]$ExePath = "build/Release/AutoLifeLiveExecutionProbe.exe",
    [string]$Market = "KRW-BTC",
    [double]$NotionalKrw = 5100.0,
    [double]$DiscountPct = 2.0,
    [int]$CancelDelayMs = 1500,
    [string]$LiveExecutionUpdatesPath = "build/Release/logs/execution_updates_live.jsonl"
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

$resolvedExePath = Resolve-RepoPath $ExePath
$resolvedLivePath = Resolve-RepoPath $LiveExecutionUpdatesPath

if (-not (Test-Path $resolvedExePath)) {
    throw "Probe executable not found: $resolvedExePath"
}

$beforeCount = 0
if (Test-Path $resolvedLivePath) {
    $beforeCount = @(Get-Content -Path $resolvedLivePath).Count
}

& $resolvedExePath `
    --market $Market `
    --notional-krw $NotionalKrw `
    --discount-pct $DiscountPct `
    --cancel-delay-ms $CancelDelayMs

if ($LASTEXITCODE -ne 0) {
    throw "Live execution probe failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $resolvedLivePath)) {
    throw "Live execution artifact not found: $resolvedLivePath"
}

$afterCount = @(Get-Content -Path $resolvedLivePath).Count
if ($afterCount -le $beforeCount) {
    throw "Live execution artifact was not appended: $resolvedLivePath"
}

Write-Host "[LiveExecutionProbe] PASSED - lines(before=$beforeCount, after=$afterCount), path=$resolvedLivePath"
exit 0
