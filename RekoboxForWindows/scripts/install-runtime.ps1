# 把 sing-box.exe / wintun.dll 复制到已安装的 Rekobox 目录
#
# 用 createDistributable 或 packageMsi 之后，内核不会自动跟着走（见 app/build.gradle.kts 的说明），
# 用这个脚本补齐。开发期不需要它 —— AppPaths 会直接找 runtime\windows。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\install-runtime.ps1 -Dest "C:\Program Files\Rekobox"
param(
    [Parameter(Mandatory = $true)]
    [string]$Dest
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "runtime\windows"

if (-not (Test-Path (Join-Path $src "sing-box.exe"))) {
    throw "没找到 $src\sing-box.exe，先跑 scripts\fetch-core.ps1"
}
if (-not (Test-Path $Dest)) {
    throw "安装目录不存在：$Dest"
}

Copy-Item (Join-Path $src "sing-box.exe") $Dest -Force
Copy-Item (Join-Path $src "wintun.dll")   $Dest -Force
Write-Host ">> 已复制内核到 $Dest"
Get-ChildItem $Dest -Filter "*.exe" | Format-Table Name, Length
