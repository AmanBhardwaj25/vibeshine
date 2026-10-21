# ChloroFrame ProRes patch installer. PowerShell 5.1, no modules.
# -Uninstall restores the backup. -DryRun only runs the version and hash checks.
# -InstallRoot and -NoService exist for the CI tests (fake install folder, no service).
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$DryRun,
    [switch]$NoService,
    [string]$InstallRoot
)

$ErrorActionPreference = 'Stop'
$LogPath = Join-Path $env:TEMP 'chloroframe-prores-patch.log'
$PackageDir = $PSScriptRoot
$ServiceName = 'SunshineService'

function Log([string]$Message) {
    Write-Host $Message
    try { Add-Content -Path $LogPath -Value ('{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message) } catch { }
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-InstallRoot {
    if ($InstallRoot) { return $InstallRoot }
    $svc = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
    if (-not $svc) { throw "$ServiceName is not installed. Install Vibeshine first." }
    $path = $svc.PathName.Trim()
    if ($path.StartsWith('"')) {
        $path = $path.Substring(1, $path.IndexOf('"', 1) - 1)
    } else {
        $path = $path -replace '(?i)^(.*?\.exe).*$', '$1'
    }
    # <root>\tools\sunshinesvc.exe
    Split-Path (Split-Path $path -Parent) -Parent
}

function Get-InstalledVersion([string]$Exe) {
    (Get-Item $Exe).VersionInfo.ProductVersion
}

function Get-BaseVersion([string]$Version) {
    if ($Version -notmatch '^\s*(\d+)\.(\d+)\.(\d+)') { throw "Cannot read a version from '$Version'." }
    '{0}.{1}.{2}' -f $Matches[1], $Matches[2], $Matches[3]
}

function Get-Sha256([string]$Path) {
    (Get-FileHash -Path $Path -Algorithm SHA256).Hash
}

function Read-Manifest {
    $path = Join-Path $PackageDir 'manifest.json'
    if (-not (Test-Path $path)) { throw "manifest.json not found next to the installer." }
    Get-Content -Path $path -Raw | ConvertFrom-Json
}

function Get-ManifestFiles($Manifest) {
    # relative path (backslashes) -> sha256
    $files = [ordered]@{}
    foreach ($p in $Manifest.files.PSObject.Properties) { $files[($p.Name -replace '/', '\')] = $p.Value }
    $files
}

function Test-Payload($Files) {
    foreach ($rel in $Files.Keys) {
        $src = Join-Path (Join-Path $PackageDir 'payload') $rel
        if (-not (Test-Path $src)) { throw "Payload file missing: $rel" }
        if ((Get-Sha256 $src) -ne $Files[$rel]) { throw "Payload hash mismatch: $rel. Download the patch again." }
        Unblock-File -Path $src
    }
}

function Remove-Shaders([string]$Root) {
    $dir = Join-Path $Root 'assets\shaders\directx'
    if (Test-Path $dir) { Get-ChildItem -Path $dir -Filter 'prores_*.hlsl' | Remove-Item -Force }
}

function Stop-Sunshine {
    if ($NoService) { return }
    $svc = Get-Service -Name $ServiceName
    if ($svc.Status -ne 'Stopped') {
        try {
            $svc.Stop()
            $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
        } catch {
            Log "Service did not stop cleanly: $($_.Exception.Message)"
        }
    }
    Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
}

function Start-Sunshine {
    if ($NoService) { return }
    Start-Service -Name $ServiceName
}

function Test-Port([int]$Port) {
    $client = New-Object Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        return ($iar.AsyncWaitHandle.WaitOne(1000) -and $client.Connected)
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

function Wait-Healthy {
    if ($NoService) { return $true }
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        if ((Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue) -and (Test-Port 47990)) { return $true }
        Start-Sleep -Seconds 1
    }
    $false
}

function Copy-WithRetry([string]$From, [string]$To) {
    New-Item -ItemType Directory -Force -Path (Split-Path $To -Parent) | Out-Null
    for ($i = 1; ; $i++) {
        try { Copy-Item -Path $From -Destination $To -Force; return } catch {
            if ($i -ge 10) { throw }
            Start-Sleep -Seconds 1
        }
    }
}

function Invoke-Install {
    if (-not ($DryRun -or $NoService) -and -not (Test-Admin)) { throw 'Run this as administrator (use Install.cmd).' }
    $manifest = Read-Manifest
    $files = Get-ManifestFiles $manifest
    $root = Get-InstallRoot
    $exe = Join-Path $root 'sunshine.exe'
    if (-not (Test-Path $exe)) { throw "sunshine.exe not found in $root" }

    $installed = Get-InstalledVersion $exe
    $base = Get-BaseVersion $installed
    if ($base -ne $manifest.vibeshineVersion) {
        throw "This patch is for Vibeshine $($manifest.vibeshineVersion), but $base is installed (product version $installed). Get the patch for your version."
    }
    Test-Payload $files

    $current = @($files.Keys | Where-Object {
        $dest = Join-Path $root $_
        (Test-Path $dest) -and ((Get-Sha256 $dest) -eq $files[$_])
    })
    if ($current.Count -eq $files.Count) {
        Log "The ProRes patch is already installed in $root."
        return
    }
    if ($DryRun) {
        Log "Dry run ok: Vibeshine $installed in $root matches the patch for $($manifest.vibeshineVersion), payload hashes verified."
        return
    }

    $backup = "$exe.chloroframe-backup"
    # Keep the first backup; refresh it only when the current exe is not ours (Vibeshine was updated in between).
    if (-not (Test-Path $backup) -or (Get-Sha256 $exe) -ne $files['sunshine.exe']) {
        Copy-Item -Path $exe -Destination $backup -Force
    }

    Log "Installing the ProRes patch into $root ..."
    try {
        Stop-Sunshine
        foreach ($rel in $files.Keys) {
            Copy-WithRetry (Join-Path (Join-Path $PackageDir 'payload') $rel) (Join-Path $root $rel)
        }
        Start-Sunshine
        if (-not (Wait-Healthy)) { throw 'Vibeshine did not come back up within 30 seconds.' }
    } catch {
        $err = $_.Exception.Message
        Log "Install failed: $err. Rolling back."
        try {
            Stop-Sunshine
            Copy-WithRetry $backup $exe
            Remove-Shaders $root
            Start-Sunshine
        } catch {
            Log "Rollback also failed: $($_.Exception.Message). Reinstall Vibeshine to repair."
        }
        throw "Rolled back ($err)"
    }
    Log 'ProRes patch installed. To undo it, run Uninstall.cmd (a Vibeshine update also removes it).'
}

function Invoke-Uninstall {
    if (-not ($DryRun -or $NoService) -and -not (Test-Admin)) { throw 'Run this as administrator (use Uninstall.cmd).' }
    $manifest = Read-Manifest
    $files = Get-ManifestFiles $manifest
    $root = Get-InstallRoot
    $exe = Join-Path $root 'sunshine.exe'
    $backup = "$exe.chloroframe-backup"
    if (-not (Test-Path $backup)) { throw 'No backup found, so there is nothing to restore. Reinstall Vibeshine to get the stock files.' }
    if ($DryRun) { Log 'Dry run ok: a backup exists.'; return }

    Log "Removing the ProRes patch from $root ..."
    Stop-Sunshine
    # If Vibeshine was updated since the install, sunshine.exe is already stock: do not downgrade it.
    if ((Get-Sha256 $exe) -eq $files['sunshine.exe']) { Copy-WithRetry $backup $exe }
    Remove-Shaders $root
    Remove-Item -Path $backup -Force
    Start-Sunshine
    Log 'ProRes patch removed. Vibeshine is back to the stock files.'
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        if ($Uninstall) { Invoke-Uninstall } else { Invoke-Install }
        exit 0
    } catch {
        Log "ERROR: $($_.Exception.Message)"
        Log "Log file: $LogPath"
        exit 1
    }
}
