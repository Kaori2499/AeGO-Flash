[CmdletBinding(SupportsShouldProcess=$true)]
param(
    [string]$AfterEffects = 'C:\Program Files\Adobe\Adobe After Effects 2026\Support Files',
    [string]$Package = (Join-Path $PSScriptRoot '..\dist\AeGOFlash')
)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path -LiteralPath $Package).Path
$hostRoot = (Resolve-Path -LiteralPath $AfterEffects).Path
if (!(Test-Path -LiteralPath (Join-Path $hostRoot 'AfterFX.exe'))) { throw 'AfterFX.exe not found in AfterEffects folder.' }
if (!(Test-Path -LiteralPath (Join-Path $sourceRoot 'AeGOFlash.aex'))) { throw 'Build AeGOFlash.aex before installing.' }
if (!(Test-Path -LiteralPath (Join-Path $sourceRoot 'FrameworkShaders'))) { throw 'FrameworkShaders folder is missing; build/install the whole package.' }
if (!(Test-Path -LiteralPath (Join-Path $sourceRoot 'scripts\TimelineClips.jsx'))) { throw 'scripts/TimelineClips.jsx is missing; install the complete package.' }
if (Get-Process AfterFX,aerender -ErrorAction SilentlyContinue) { throw 'Close After Effects and aerender before installing. Your open work has not been changed.' }
$legacyRoot = Join-Path $hostRoot 'Plug-ins\Live2DNativeAE'
if (Test-Path -LiteralPath $legacyRoot) {
    throw 'Move the existing Live2DNativeAE folder outside Plug-ins as a backup before installing AeGO Flash. Both share the same saved-project identity.'
}
$targetRoot = Join-Path $hostRoot 'Plug-ins\AeGOFlash'
if (Test-Path -LiteralPath $targetRoot) {
    throw "Target already exists: $targetRoot. Keep a backup and choose a fresh target before replacing an existing version."
}
if ($PSCmdlet.ShouldProcess($targetRoot, 'Install AeGO Flash plugin and shaders')) {
    New-Item -ItemType Directory -Path $targetRoot | Out-Null
    Get-ChildItem -LiteralPath $sourceRoot | Copy-Item -Destination $targetRoot -Recurse
    Write-Host 'Installed. Open AE and find Effects > AeGO > AeGO Flash.'
}
