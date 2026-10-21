ChloroFrame ProRes patch for Vibeshine: adds an experimental GPU ProRes encoder (replaces sunshine.exe, adds prores_*.hlsl shaders).
To install: double-click Install.cmd and accept the UAC prompt. It backs up sunshine.exe and rolls back by itself if Vibeshine does not start.
To undo: double-click Uninstall.cmd (restores the backup and removes the shaders).
This patch only fits the Vibeshine version named in manifest.json; the installer refuses any other version.
Updating Vibeshine removes the patch (ChloroFrame then falls back to HEVC). Log: %TEMP%\chloroframe-prores-patch.log
