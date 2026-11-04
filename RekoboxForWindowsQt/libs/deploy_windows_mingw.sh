#!/usr/bin/env bash
# 组装 Windows 部署目录（MinGW 版，不需要 MSVC / windeployqt 的系统组件）
# 产物：deployment/windows64/
set -euo pipefail
cd "$(dirname "$0")/.."

export PATH="/d/Qt/6.7.2/mingw_64/bin:/d/Qt/Tools/mingw1310_64/bin:$PATH"
export TMP="D:/WorkBuddy/Rekobox/.tmp" TEMP="D:/WorkBuddy/Rekobox/.tmp"

BUILD="$PWD/build"
DEST="$PWD/deployment/windows64"
MINGW="/d/Qt/Tools/mingw1310_64/bin"

rm -rf "$DEST"
mkdir -p "$DEST"

echo ">> [1/5] 主程序与内核"
cp "$BUILD/rekobox.exe" "$DEST/"
cp "$PWD/deployment/core/rekobox_core.exe" "$DEST/"

echo ">> [2/5] Qt 运行时"
pushd "$DEST" > /dev/null
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw \
    --compiler-runtime rekobox.exe > /dev/null
# MinGW 运行时兜底
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    [ -f "$dll" ] || cp "$MINGW/$dll" .
done
rm -rf libEGL.dll libGLESv2.dll 2>/dev/null || true
popd > /dev/null

echo ">> [3/5] 分流规则数据"
GEODIR="$PWD/res/_geodata"
mkdir -p "$GEODIR"
fetch() { [ -f "$2" ] || curl -fLso "$2" "$1"; }
fetch "https://github.com/SagerNet/sing-geoip/releases/latest/download/geoip.db"   "$GEODIR/geoip.db"
fetch "https://github.com/SagerNet/sing-geosite/releases/latest/download/geosite.db" "$GEODIR/geosite.db"
fetch "https://github.com/Loyalsoldier/v2ray-rules-dat/releases/latest/download/geoip.dat" "$GEODIR/geoip.dat"
fetch "https://github.com/v2fly/domain-list-community/releases/latest/download/dlc.dat"    "$GEODIR/geosite.dat"
cp "$GEODIR"/*.db "$GEODIR"/*.dat "$DEST/" 2>/dev/null || true
cp "$PWD/res/public/"* "$DEST/"

echo ">> [4/5] TUN 驱动"
WINTUN="$PWD/res/_geodata/wintun.dll"
if [ ! -f "$WINTUN" ]; then
    curl -fLso "$WINTUN.zip" https://www.wintun.net/builds/wintun-0.14.1.zip
    unzip -o -j "$WINTUN.zip" "wintun/bin/amd64/wintun.dll" -d "$(dirname "$WINTUN")" > /dev/null
fi
cp "$WINTUN" "$DEST/"

echo ">> [5/5] 完成"
du -sh "$DEST"
ls "$DEST" | head -30
