<#
.SYNOPSIS
  Downloads Dear ImGui v1.91.9 (MIT, github.com/ocornut/imgui) into third_party/ for the menu mod.
  The archive hash is pinned; the download is refused if it does not match.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tp = Join-Path $root 'third_party'
$zip = Join-Path $tp 'imgui-1.91.9.zip'
$sha = '3fe9e93066e37c630cbbada821a45812cd86df0f21a476cf74c925e1b649878d'
New-Item -ItemType Directory -Force $tp | Out-Null
if (Test-Path (Join-Path $tp 'imgui-1.91.9\imgui.h')) { Write-Host 'Dear ImGui 1.91.9 already present'; return }
Invoke-WebRequest 'https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.zip' -OutFile $zip -UseBasicParsing
$got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($got -ne $sha) { Remove-Item $zip; throw "unexpected archive hash $got" }
Expand-Archive $zip -DestinationPath $tp -Force
Remove-Item $zip
Write-Host "Dear ImGui 1.91.9 -> $tp\imgui-1.91.9"
