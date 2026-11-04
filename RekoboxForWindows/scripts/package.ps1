# 构建 Windows 安装包
#
# 用法（在 RekoboxForWindows 根目录）：
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.0.1
#
# 前置：WiX Toolset 3.x 在 PATH 中（https://github.com/wixtoolset/wix3/releases），
#       JDK 17+，runtime\windows 里已有 sing-box.exe。
param(
    [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path "runtime\windows\sing-box.exe")) {
    Write-Host ">> 缺少内核，先执行 fetch-core.ps1"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "fetch-core.ps1")
}

Write-Host ">> 跑单元测试"
& .\gradlew.bat :core:test --console=plain
if ($LASTEXITCODE -ne 0) { throw "单元测试失败" }

Write-Host ">> 打包 MSI"
& .\gradlew.bat :app:packageMsi --console=plain
if ($LASTEXITCODE -ne 0) { throw "打包失败" }

$msi = Get-ChildItem "app\build\compose\binaries\main\msi" -Filter "*.msi" | Select-Object -First 1
$out = Join-Path (Split-Path -Parent $root) "dist"
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item $msi.FullName (Join-Path $out "Rekobox-$Version-windows-x64.msi") -Force

Write-Host ">> 产物：$(Join-Path $out "Rekobox-$Version-windows-x64.msi")"
Write-Host ">> 安装后如果启动报找不到内核，执行："
Write-Host "   scripts\install-runtime.ps1 -Dest `"C:\Program Files\Rekobox`""
