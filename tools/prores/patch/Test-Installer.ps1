# CI test: builds a patch from fake files and runs the installer against a fake install folder.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$work = Join-Path ([IO.Path]::GetTempPath()) ('cfprores-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$failures = 0

function Check([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "PASS $Name" } else { Write-Host "FAIL $Name"; $script:failures++ }
}
function Write-Text([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
function Sha([string]$Path) { (Get-FileHash $Path -Algorithm SHA256).Hash }
function Throws([scriptblock]$Block, [string]$Pattern) {
    try { & $Block } catch { return ($_.Exception.Message -match $Pattern) }
    $false
}

# Fake package.
$shaderSrc = Join-Path $work 'shaders'
Write-Text (Join-Path $shaderSrc 'prores_common.hlsl') 'common'
Write-Text (Join-Path $shaderSrc 'prores_dct_cs.hlsl') 'dct'
Write-Text (Join-Path $shaderSrc 'other.hlsl') 'not ours'
Write-Text (Join-Path $work 'patched.exe') 'patched build'
$pkg = Join-Path $work 'pkg'
& (Join-Path $here 'New-PatchPackage.ps1') -SunshineExe (Join-Path $work 'patched.exe') -ShaderSource $shaderSrc `
    -OutDir $pkg -BaseCommit 'abc123' -VibeshineVersion '9.9.9' -ZipPath (Join-Path $work 'pkg.zip')
Check 'package has the expected files' ((Test-Path (Join-Path $pkg 'manifest.json')) -and (Test-Path (Join-Path $pkg 'Install.cmd')) -and
    (Test-Path (Join-Path $pkg 'payload\assets\shaders\directx\prores_dct_cs.hlsl')) -and -not (Test-Path (Join-Path $pkg 'payload\assets\shaders\directx\other.hlsl')))

# Load the installer functions without running it, then fake the version lookup.
. (Join-Path $here 'Install-ProResPatch.ps1')
$InstallRoot = Join-Path $work 'install'
$NoService = $true
$PackageDir = $pkg
$LogPath = Join-Path $work 'test.log'
$script:FakeVersion = '9.9.9.123'
function Get-InstalledVersion([string]$Exe) { $script:FakeVersion }

$exe = Join-Path $InstallRoot 'sunshine.exe'
$backup = "$exe.chloroframe-backup"
$shaderDir = Join-Path $InstallRoot 'assets\shaders\directx'
Write-Text $exe 'stock build'
Write-Text (Join-Path $InstallRoot 'tools\sunshinesvc.exe') 'svc'
Write-Text (Join-Path $shaderDir 'other.hlsl') 'not ours'
$stockHash = Sha $exe
$patchedHash = Sha (Join-Path $work 'patched.exe')

$DryRun = $true
Invoke-Install
Check 'dry run changes nothing' ((Sha $exe) -eq $stockHash -and -not (Test-Path $backup))
$DryRun = $false

Invoke-Install
Check 'install replaces sunshine.exe' ((Sha $exe) -eq $patchedHash)
Check 'install keeps a backup of the original' ((Test-Path $backup) -and (Sha $backup) -eq $stockHash)
Check 'install adds the shaders' ((Test-Path (Join-Path $shaderDir 'prores_common.hlsl')) -and (Test-Path (Join-Path $shaderDir 'prores_dct_cs.hlsl')))

Invoke-Install
Check 'second install is harmless and keeps the original backup' ((Sha $exe) -eq $patchedHash -and (Sha $backup) -eq $stockHash)

Invoke-Uninstall
Check 'uninstall restores the original' ((Sha $exe) -eq $stockHash -and -not (Test-Path $backup))
Check 'uninstall removes only our shaders' (-not (Test-Path (Join-Path $shaderDir 'prores_common.hlsl')) -and (Test-Path (Join-Path $shaderDir 'other.hlsl')))

$script:FakeVersion = '1.0.0.0'
Check 'version mismatch is refused' (Throws { Invoke-Install } 'patch is for Vibeshine 9\.9\.9')
Check 'version mismatch changes nothing' ((Sha $exe) -eq $stockHash -and -not (Test-Path $backup))
$script:FakeVersion = '9.9.9.123'

$payloadFile = Join-Path $pkg 'payload\assets\shaders\directx\prores_dct_cs.hlsl'
Write-Text $payloadFile 'tampered'
Check 'tampered payload is refused' (Throws { Invoke-Install } 'hash mismatch')
Check 'tampered payload changes nothing' ((Sha $exe) -eq $stockHash -and -not (Test-Path $backup))
Write-Text $payloadFile 'dct'

# Vibeshine updated between install and a second install: the backup must follow the new stock exe.
Invoke-Install
Write-Text $exe 'stock build 2'
$stock2 = Sha $exe
Invoke-Install
Check 'reinstall after an update refreshes the backup' ((Sha $backup) -eq $stock2 -and (Sha $exe) -eq $patchedHash)
Invoke-Uninstall

# Update after install, then uninstall: do not downgrade the new exe.
Invoke-Install
Write-Text $exe 'stock build 3'
$stock3 = Sha $exe
Invoke-Uninstall
Check 'uninstall after an update keeps the new exe' ((Sha $exe) -eq $stock3 -and -not (Test-Path $backup))

Remove-Item -Recurse -Force $work
if ($failures -gt 0) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'All installer checks passed'
