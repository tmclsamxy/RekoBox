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
# .dat 仅供界面做分类名自动补全（本地读取，不进核心）
fetch "https://github.com/Loyalsoldier/v2ray-rules-dat/releases/latest/download/geoip.dat" "$GEODIR/geoip.dat"
fetch "https://github.com/v2fly/domain-list-community/releases/latest/download/dlc.dat"    "$GEODIR/geosite.dat"

# sing-box 1.12 起 geoip:/geosite: rule 已移除，改为 rule_set 引用 .srs 数据。
# 常用集合随包分发（离线可用）；配置里用到但本地没有的集合，会自动回退到远程地址。
SRSDIR="$GEODIR/srs"
mkdir -p "$SRSDIR"
fetch_srs() {
    [ -f "$SRSDIR/$1.srs" ] && return 0
    curl -fsL --retry 3 -o "$SRSDIR/$1.srs" \
        "https://raw.githubusercontent.com/SagerNet/sing-${1%%-*}/rule-set/$1.srs" \
        || rm -f "$SRSDIR/$1.srs"
}
for tag in geosite-cn geosite-geolocation-cn "geosite-geolocation-!cn" geosite-category-ads-all \
           geosite-google geosite-telegram geosite-youtube geoip-cn geoip-hk geoip-tw geoip-jp geoip-sg geoip-us; do
    fetch_srs "$tag"
done

cp "$GEODIR"/*.dat "$DEST/" 2>/dev/null || true
cp "$SRSDIR"/*.srs "$DEST/" 2>/dev/null || true
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
