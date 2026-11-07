<#
.SYNOPSIS
  Installs Plugin_ModKit.dll into Sacred 2 Gold and registers it in system/plugins.cfg.

.DESCRIPTION
  Every file it replaces gets a backup (<file>.bak-<date>). It also removes the old
  Plugin_AutoAttack line (the ModKit replaces it). -Camera also installs the camera,
  ultrawide and no-fog settings (config/optionsCustom.txt) into the profile folder.

.EXAMPLE
  .\scripts\install.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Sacred 2 Gold" -Camera
#>
param(
    [string]$GameDir,
    [switch]$Camera
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

function Find-Game {
    $candidates = @()
    foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKCU:\SOFTWARE\Valve\Steam') {
        $p = (Get-ItemProperty $k -ErrorAction SilentlyContinue).InstallPath
        if (-not $p) { $p = (Get-ItemProperty $k -ErrorAction SilentlyContinue).SteamPath }
        if ($p) {
            $candidates += Join-Path $p 'steamapps\common\Sacred 2 Gold'
            $lib = Join-Path $p 'steamapps\libraryfolders.vdf'
            if (Test-Path $lib) {
                foreach ($m in [regex]::Matches((Get-Content $lib -Raw), '"path"\s+"([^"]+)"')) {
                    $candidates += Join-Path ($m.Groups[1].Value -replace '\\\\', '\') 'steamapps\common\Sacred 2 Gold'
                }
            }
        }
    }
    $candidates | Where-Object { Test-Path (Join-Path $_ 'system\plugins.cfg') } | Select-Object -First 1
}

if (-not $GameDir) { $GameDir = Find-Game }
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir 'system\plugins.cfg'))) {
    throw 'Sacred 2 Gold not found: pass -GameDir "<folder with system\plugins.cfg>"'
}
if (Get-Process sacred2 -ErrorAction SilentlyContinue) { throw 'Close Sacred 2 first.' }
$sys = Join-Path $GameDir 'system'
$dll = Join-Path $root 'build\Plugin_ModKit.dll'
if (-not (Test-Path $dll)) { throw 'build\Plugin_ModKit.dll missing: run .\build.ps1 first' }

function Backup($path) {
    if (Test-Path $path) { Copy-Item $path "$path.bak-$stamp"; Write-Host "backup: $path.bak-$stamp" }
}

Backup (Join-Path $sys 'Plugin_ModKit.dll')
Copy-Item $dll $sys -Force
Write-Host "installed: $sys\Plugin_ModKit.dll"

$cfg = Join-Path $sys 'plugins.cfg'
$lines = Get-Content $cfg
$new = @($lines | Where-Object { $_ -notmatch '^\s*Plugin\s*=\s*Plugin_AutoAttack\s*$' })
if (-not ($new | Where-Object { $_ -match '^\s*Plugin\s*=\s*Plugin_ModKit\s*$' })) {
    $at = [array]::FindLastIndex([string[]]$new, [Predicate[string]] { param($l) $l -match '^\s*Plugin\s*=\s*Plugin_' -and $l -notmatch 'openal' })
    if ($at -lt 0) { $new += 'Plugin=Plugin_ModKit' }
    else { $new = @($new[0..$at]) + 'Plugin=Plugin_ModKit' + @($new[($at + 1)..($new.Count - 1)]) }
}
if (($new -join "`n") -ne ($lines -join "`n")) {
    Backup $cfg
    $new | Set-Content -Encoding ascii $cfg
    Write-Host 'plugins.cfg: Plugin=Plugin_ModKit registered (Plugin_AutoAttack removed)'
}

if ($Camera) {
    $prof = Join-Path $env:LOCALAPPDATA 'Ascaron Entertainment\Sacred 2'
    New-Item -ItemType Directory -Force $prof | Out-Null
    $dst = Join-Path $prof 'optionsCustom.txt'
    Backup $dst
    Copy-Item (Join-Path $root 'config\optionsCustom.txt') $dst -Force
    Write-Host "camera/ultrawide/no-fog: $dst"
}
Write-Host 'Done. In the game: Insert = automation ON/OFF, Home = menu, End = diagnostic dump.'
