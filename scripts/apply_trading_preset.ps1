param(
    [ValidateSet("safe", "active")]
    [string]$Preset = "safe",
    [string]$PresetPath = "",
    [string]$ConfigPath = ".\build\Release\config\config.json",
    [string]$SourceConfigPath = ".\config\config.json"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Ensure-ParentDirectory {
    param([string]$PathValue)
    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path $parent)) {
        New-Item -Path $parent -ItemType Directory -Force | Out-Null
    }
}

function Set-OrAddProperty {
    param(
        $ObjectValue,
        [string]$Name,
        $Value
    )
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

function Merge-JsonObject {
    param(
        $Target,
        $Patch
    )
    if ($null -eq $Patch) {
        return $Target
    }

    foreach ($prop in $Patch.PSObject.Properties) {
        $name = [string]$prop.Name
        $value = $prop.Value

        $targetProp = $Target.PSObject.Properties[$name]
        if ($value -is [pscustomobject]) {
            if ($null -eq $targetProp -or -not ($targetProp.Value -is [pscustomobject])) {
                Set-OrAddProperty -ObjectValue $Target -Name $name -Value ([pscustomobject]@{})
                $targetProp = $Target.PSObject.Properties[$name]
            }
            Merge-JsonObject -Target $targetProp.Value -Patch $value | Out-Null
        } else {
            Set-OrAddProperty -ObjectValue $Target -Name $name -Value $value
        }
    }

    return $Target
}

if ([string]::IsNullOrWhiteSpace($PresetPath)) {
    $PresetPath = ".\config\presets\$Preset.json"
}

if (-not (Test-Path $PresetPath)) {
    throw "Preset file not found: $PresetPath"
}

if (-not (Test-Path $ConfigPath)) {
    if (-not (Test-Path $SourceConfigPath)) {
        throw "Config not found and source config missing: $ConfigPath / $SourceConfigPath"
    }
    Ensure-ParentDirectory -PathValue $ConfigPath
    Copy-Item -Path $SourceConfigPath -Destination $ConfigPath -Force
}

$config = Get-Content -Raw $ConfigPath | ConvertFrom-Json -ErrorAction Stop
$presetRoot = Get-Content -Raw $PresetPath | ConvertFrom-Json -ErrorAction Stop

$patch = [pscustomobject]@{}
if ($presetRoot.PSObject.Properties["trading"]) {
    Set-OrAddProperty -ObjectValue $patch -Name "trading" -Value $presetRoot.trading
}
if ($presetRoot.PSObject.Properties["strategies"]) {
    Set-OrAddProperty -ObjectValue $patch -Name "strategies" -Value $presetRoot.strategies
}

Merge-JsonObject -Target $config -Patch $patch | Out-Null
($config | ConvertTo-Json -Depth 32) | Set-Content -Path $ConfigPath -Encoding UTF8

$presetName = if ($presetRoot.PSObject.Properties["preset_name"]) { [string]$presetRoot.preset_name } else { $Preset }
Write-Host "[ApplyTradingPreset] Applied preset: $presetName"
Write-Host "config_path=$ConfigPath"
