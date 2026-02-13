param(
    [string]$ExePath = "build/Release/AutoLifeTrading.exe",
    [switch]$IncludeBacktest,
    [switch]$RunLiveProbe,
    [switch]$StrictExecutionParity,
    [string]$BacktestPrimeCsv = "data/backtest/simulation_large.csv",
    [string]$FixtureLogDir = "build/Release/logs/ci_fixture",
    [string]$ProbeMarket = "KRW-BTC",
    [double]$ProbeNotionalKrw = 5100.0,
    [double]$ProbeDiscountPct = 2.0,
    [int]$ProbeCancelDelayMs = 1500
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
$resolvedBacktestPrimeCsv = Resolve-RepoPath $BacktestPrimeCsv
$resolvedFixtureLogDir = Resolve-RepoPath $FixtureLogDir

if (-not (Test-Path $resolvedExePath)) {
    throw "Executable not found: $resolvedExePath"
}
if (-not (Test-Path $resolvedBacktestPrimeCsv)) {
    throw "Backtest prime dataset not found: $resolvedBacktestPrimeCsv"
}

$fixtureScript = Resolve-RepoPath "scripts/prepare_operational_readiness_fixture.ps1"
$probeScript = Resolve-RepoPath "scripts/generate_live_execution_probe.ps1"
$readinessScript = Resolve-RepoPath "scripts/validate_readiness.ps1"
$operationalScript = Resolve-RepoPath "scripts/validate_operational_readiness.ps1"

if (-not (Test-Path $fixtureScript)) { throw "Missing script: $fixtureScript" }
if (-not (Test-Path $readinessScript)) { throw "Missing script: $readinessScript" }
if (-not (Test-Path $operationalScript)) { throw "Missing script: $operationalScript" }
if ($RunLiveProbe.IsPresent -and -not (Test-Path $probeScript)) { throw "Missing script: $probeScript" }

$fixtureLogPath = Join-Path $resolvedFixtureLogDir "autolife_ci_fixture.log"
& powershell -NoProfile -ExecutionPolicy Bypass -File $fixtureScript -LogPath $fixtureLogPath
if ($LASTEXITCODE -ne 0) {
    throw "Fixture preparation failed with exit code $LASTEXITCODE"
}

if ($IncludeBacktest.IsPresent) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $readinessScript -ExePath $resolvedExePath -OutputJson "build/Release/logs/readiness_report_ci_prewarm.json"
    if ($LASTEXITCODE -ne 0) {
        throw "Backtest prewarm failed with exit code $LASTEXITCODE"
    }
}

# Prime backtest execution artifact immediately before operational readiness parity checks.
& $resolvedExePath --backtest $resolvedBacktestPrimeCsv --json | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Backtest execution artifact prime run failed with exit code $LASTEXITCODE"
}
$backtestArtifactPath = Resolve-RepoPath "build/Release/logs/execution_updates_backtest.jsonl"
if (-not (Test-Path $backtestArtifactPath)) {
    throw "Backtest execution artifact not found: $backtestArtifactPath"
}
$backtestArtifactLines = @(
    Get-Content -Path $backtestArtifactPath |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
).Count
if ($backtestArtifactLines -le 0) {
    throw "Backtest execution artifact is empty after prime run: $backtestArtifactPath"
}

if ($RunLiveProbe.IsPresent) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $probeScript `
        -Market $ProbeMarket `
        -NotionalKrw $ProbeNotionalKrw `
        -DiscountPct $ProbeDiscountPct `
        -CancelDelayMs $ProbeCancelDelayMs

    if ($LASTEXITCODE -ne 0) {
        throw "Live probe failed with exit code $LASTEXITCODE"
    }
}

$args = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $operationalScript,
    "-StrictLogCheck",
    "-LogDir", $resolvedFixtureLogDir
)

if ($IncludeBacktest.IsPresent) {
    $args += "-IncludeBacktest"
}
if ($StrictExecutionParity.IsPresent) {
    $args += "-StrictExecutionParity"
}

& powershell @args
if ($LASTEXITCODE -ne 0) {
    throw "Operational readiness gate failed with exit code $LASTEXITCODE"
}

Write-Host "[CIGate] PASSED"
exit 0
