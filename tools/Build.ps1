[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AeSdk,
    [Parameter(Mandatory=$true)][string]$CubismSdk,
    [string]$CMake = 'cmake',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$aeRoot = (Resolve-Path -LiteralPath $AeSdk).Path
$cubismRoot = (Resolve-Path -LiteralPath $CubismSdk).Path
if (!(Test-Path -LiteralPath (Join-Path $aeRoot 'Examples\Headers\AE_Effect.h'))) {
    throw 'AeSdk must point at the SDK root containing Examples\Headers\AE_Effect.h.'
}
if (!(Test-Path -LiteralPath (Join-Path $cubismRoot 'Core\include\Live2DCubismCore.h'))) {
    throw 'CubismSdk must point at CubismSdkForNative-5-r.5 (Core and Framework folders).'
}
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswherePath)) {
    throw 'Visual Studio 2022 Build Tools are required. Install Desktop development with C++ and Windows SDK, then rerun.'
}
$vsInstall = & $vswherePath -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall) { throw 'Visual Studio 2022 C++ x64 toolset v143 was not found.' }
if ($CMake -eq 'cmake' -and !(Get-Command cmake -ErrorAction SilentlyContinue)) {
    $CMake = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (!(Get-Command $CMake -ErrorAction SilentlyContinue)) { throw 'CMake was not found. Supply -CMake with the path to cmake.exe.' }
$buildRoot = Join-Path $projectRoot 'build'
& $CMake -S $projectRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64 "-DAE_SDK_ROOT=$aeRoot" "-DCUBISM_SDK_ROOT=$cubismRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $CMake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed. No install was performed.' }
& $CMake --install $buildRoot --config $Configuration --prefix (Join-Path $projectRoot 'dist')
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
Write-Host "Build complete. Review the files in $(Join-Path $projectRoot 'dist')."

