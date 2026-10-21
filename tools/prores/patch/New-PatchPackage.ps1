# Builds the patch folder (and optionally the zip) from a sunshine.exe and the shader sources.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SunshineExe,
    [Parameter(Mandatory)][string]$ShaderSource,
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][string]$BaseCommit,
    [string]$VibeshineVersion,
    [string]$ZipPath
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

if (-not $VibeshineVersion) {
    $product = (Get-Item $SunshineExe).VersionInfo.ProductVersion
    if ($product -notmatch '^\s*(\d+)\.(\d+)\.(\d+)') { throw "Cannot read a version from '$product'." }
    $VibeshineVersion = '{0}.{1}.{2}' -f $Matches[1], $Matches[2], $Matches[3]
}

$shaders = @(Get-ChildItem -Path $ShaderSource -Filter 'prores_*.hlsl')
if ($shaders.Count -eq 0) { throw "No prores_*.hlsl in $ShaderSource" }

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
$shaderOut = Join-Path $OutDir 'payload\assets\shaders\directx'
New-Item -ItemType Directory -Force -Path $shaderOut | Out-Null
Copy-Item $SunshineExe (Join-Path $OutDir 'payload\sunshine.exe')
foreach ($s in $shaders) { Copy-Item $s.FullName $shaderOut }
foreach ($f in 'Install.cmd', 'Uninstall.cmd', 'Install-ProResPatch.ps1', 'README.txt') {
    Copy-Item (Join-Path $here $f) $OutDir
}

$files = [ordered]@{}
$files['sunshine.exe'] = (Get-FileHash (Join-Path $OutDir 'payload\sunshine.exe') -Algorithm SHA256).Hash
foreach ($s in ($shaders | Sort-Object Name)) {
    $files["assets/shaders/directx/$($s.Name)"] = (Get-FileHash $s.FullName -Algorithm SHA256).Hash
}
$manifest = [ordered]@{ vibeshineVersion = $VibeshineVersion; baseCommit = $BaseCommit; files = $files }
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $OutDir 'manifest.json') -Encoding UTF8

if ($ZipPath) {
    if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
    Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $ZipPath
}
Write-Host "Patch for Vibeshine $VibeshineVersion built in $OutDir"
