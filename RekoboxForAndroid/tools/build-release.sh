#!/usr/bin/env bash
# RekoBox Android Release 构建脚本（Windows / Git Bash）
#
# 前置：
#   1. Go 1.25+            D:/rea-tools/go
#   2. gomobile-matsuri    GOPATH/bin（见 libcore/init.sh 的安装方式）
#   3. Android SDK + NDK   路径见 local.properties
#   4. sing-box / libneko  源码在仓库父目录（libcore/go.mod 里 ../../ 的 replace）
#   5. local.properties    含 sdk.dir / ndk.dir / KEYSTORE_PASS / ALIAS_NAME / ALIAS_PASS
#
# 用法：bash tools/build-release.sh [oss|fdroid|play|preview]
set -euo pipefail
cd "$(dirname "$0")/.."

FLAVOR="${1:-oss}"

export GOPATH="${GOPATH:-D:/WorkBuddy/Rekobox/.gopath}"
export GOCACHE="${GOCACHE:-D:/WorkBuddy/Rekobox/.gocache}"
export GOPROXY="${GOPROXY:-https://goproxy.cn,direct}"
export GOTOOLCHAIN=local
export PATH="$GOPATH/bin:/d/rea-tools/gopath/bin:/d/rea-tools/go/bin:$PATH"

# 构建临时目录必须放在工作区内：Go/Gradle 在 %TEMP% 下会撞权限限制
export TMP="D:/WorkBuddy/Rekobox/.tmp" TEMP="D:/WorkBuddy/Rekobox/.tmp"
export TMPDIR="/d/WorkBuddy/Rekobox/.tmp"
mkdir -p "$TMPDIR"

export ANDROID_HOME="${ANDROID_HOME:-D:/Android/Sdk}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/28.2.13676358}"
export GOBIND=gobind-matsuri

# Windows 上 AGP 新版资源压缩器在 ABI 分包时并发写同一个 resources.txt 会报“拒绝访问”，
# 只关资源压缩，R8 混淆与裁剪仍然生效。
export REKOBOX_NO_RES_SHRINK="${REKOBOX_NO_RES_SHRINK:-1}"

echo "==> [1/3] libcore.aar"
if [ ! -f app/libs/libcore.aar ]; then
    mkdir -p app/libs
    ( cd libcore && gomobile-matsuri bind \
        -androidapi 21 -cache "$(pwd)/.build" -trimpath -ldflags='-s -w' \
        -tags='with_conntrack,with_gvisor,with_quic,with_wireguard,with_utls,with_clash_api' . )
    cp -f libcore/libcore.aar app/libs/
else
    echo "    已存在，跳过"
fi

echo "==> [2/3] libhev-socks5-tunnel.so"
if [ ! -f app/src/main/jniLibs/arm64-v8a/libhev-socks5-tunnel.so ]; then
    bash buildScript/compile-hevtun.sh
else
    echo "    已存在，跳过"
fi

echo "==> [3/3] Gradle assemble${FLAVOR}Release"
./gradlew "app:assemble${FLAVOR^}Release" --console=plain

echo
echo "==> 产物："
find app/build/outputs/apk -name "*.apk" -print0 | xargs -0 ls -lh
