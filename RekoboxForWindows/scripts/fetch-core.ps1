# 下载 sing-box 内核与 Wintun 驱动到 runtime/windows
#
# 用法（在 RekoBoxForWin 根目录）：
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-core.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-core.ps1 -Version 1.10.7
param(
    [string]$Version = "1.10.7",
    [string]$Arch = "amd64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "runtime\windows"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$tmp = Join-Path $env:TEMP "rekobox-core-$Version"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

Write-Host ">> 下载 sing-box $Version ($Arch)"
$url = "https://github.com/SagerNet/sing-box/releases/download/v$Version/sing-box-$Version-windows-$Arch.zip"
Invoke-WebRequest -Uri $url -OutFile "$tmp\sing-box.zip"
Expand-Archive -Path "$tmp\sing-box.zip" -DestinationPath $tmp -Force

$exe = Get-ChildItem -Path $tmp -Recurse -Filter "sing-box.exe" | Select-Object -First 1
Copy-Item $exe.FullName (Join-Path $dest "sing-box.exe") -Force

Write-Host ">> 下载 Wintun"
$wintun = "https://www.wintun.net/builds/wintun-0.14.1.zip"
Invoke-WebRequest -Uri $wintun -OutFile "$tmp\wintun.zip"
Expand-Archive -Path "$tmp\wintun.zip" -DestinationPath "$tmp\wintun" -Force
Copy-Item "$tmp\wintun\wintun\bin\$Arch\wintun.dll" (Join-Path $dest "wintun.dll") -Force

Get-ChildItem $dest | Format-Table Name, Length
Write-Host ">> 完成：$dest"
