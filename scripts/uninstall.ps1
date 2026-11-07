<#
.SYNOPSIS
  Unregisters the ModKit: removes the Plugin=Plugin_ModKit line from system/plugins.cfg
  (with a backup). The DLL and its ini stay in system/ so nothing is lost; delete them
  by hand if you want. -Camera restores the newest optionsCustom.txt backup.
#>
param(
    [Parameter(Mandatory)][string]$GameDir,
    [switch]$Camera
)
$ErrorActionPreference = 'Stop'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$cfg = Join-Path $GameDir 'system\plugins.cfg'
$lines = Get-Content $cfg
$new = $lines | Where-Object { $_ -notmatch '^\s*Plugin\s*=\s*Plugin_ModKit\s*$' }
if (@($new).Count -ne @($lines).Count) {
    Copy-Item $cfg "$cfg.bak-$stamp"
    $new | Set-Content -Encoding ascii $cfg
    Write-Host "plugins.cfg: ModKit unregistered (backup $cfg.bak-$stamp)"
} else {
    Write-Host 'plugins.cfg: ModKit was not registered'
}
if ($Camera) {
    $dst = Join-Path $env:LOCALAPPDATA 'Ascaron Entertainment\Sacred 2\optionsCustom.txt'
    $bak = Get-ChildItem "$dst.bak-*" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($bak) { Copy-Item $bak.FullName $dst -Force; Write-Host "restored $($bak.Name)" }
    else { Write-Host 'no optionsCustom.txt backup found; delete the file to return to the game defaults' }
}
