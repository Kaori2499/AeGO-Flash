[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Model)
$ErrorActionPreference = 'Stop'
$modelPath = (Resolve-Path -LiteralPath $Model).Path
if (!$modelPath.EndsWith('.model3.json', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Select an exported .model3.json file.'
}
$modelDirectory = Split-Path -Parent $modelPath
$data = Get-Content -LiteralPath $modelPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($data.Version -ne 3 -or !$data.FileReferences -or !$data.FileReferences.Moc) {
    throw 'Invalid Cubism model3 schema: Version 3 and FileReferences.Moc are required.'
}
$files = [Collections.Generic.List[object]]::new()
function Add-ReferencedFile([string]$Kind, [string]$RelativePath) {
    if ([string]::IsNullOrWhiteSpace($RelativePath)) { return }
    $resolvedFile = if ([IO.Path]::IsPathRooted($RelativePath)) {
        [IO.Path]::GetFullPath($RelativePath)
    } else { [IO.Path]::GetFullPath((Join-Path $modelDirectory $RelativePath)) }
    $files.Add([PSCustomObject]@{
        Kind = $Kind
        File = $resolvedFile
        Exists = (Test-Path -LiteralPath $resolvedFile -PathType Leaf)
    })
}
Add-ReferencedFile 'Moc' $data.FileReferences.Moc
foreach ($texture in $data.FileReferences.Textures) { Add-ReferencedFile 'Texture' $texture }
if (!$data.FileReferences.Textures -or @($data.FileReferences.Textures).Count -eq 0) { throw 'Model has no textures.' }
Add-ReferencedFile 'Physics' $data.FileReferences.Physics
Add-ReferencedFile 'Pose' $data.FileReferences.Pose
foreach ($group in $data.FileReferences.Motions.PSObject.Properties) {
    foreach ($motion in $group.Value) { Add-ReferencedFile 'Motion' $motion.File }
}
$missing = @($files | Where-Object { !$_.Exists })
if ($missing.Count) {
    $missing | Format-Table Kind,File -Wrap | Out-Host
    throw "Model package has $($missing.Count) missing referenced files."
}
$moc = [IO.File]::ReadAllBytes(($files | Where-Object Kind -eq 'Moc' | Select-Object -First 1).File)
if ($moc.Length -lt 64 -or [Text.Encoding]::ASCII.GetString($moc, 0, 4) -ne 'MOC3') {
    throw 'The referenced moc3 file does not have a valid MOC3 header.'
}
[PSCustomObject]@{
    Model = $modelPath
    Textures = @($files | Where-Object Kind -eq 'Texture').Count
    Motions = @($files | Where-Object Kind -eq 'Motion').Count
    Physics = [bool]$data.FileReferences.Physics
    Pose = [bool]$data.FileReferences.Pose
    MissingFiles = $missing.Count
    Result = 'File-reference validation passed; rendering requires the native build.'
}
