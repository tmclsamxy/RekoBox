#!/usr/bin/env bash
# RekoBox for Windows (Qt) —— 一键构建
#
# 前置（一次性）：
#   Qt 6.7.2 mingw_64 + MinGW 13.1      → D:/Qt
#   CMake 3.16+ / Ninja                 → pip install cmake ninja
#   Go 1.25+                            → D:/rea-tools/go
#
# 用法：bash libs/build_all_mingw.sh [--skip-deps]
set -euo pipefail
cd "$(dirname "$0")/.."

export PATH="/d/Qt/6.7.2/mingw_64/bin:/d/Qt/Tools/mingw1310_64/bin:/d/rea-tools/go/bin:/c/Users/Mrcheng/AppData/Local/Programs/Python/Python310/Scripts:$PATH"
export CC=gcc CXX=g++
export TMP="D:/WorkBuddy/Rekobox/.tmp" TEMP="D:/WorkBuddy/Rekobox/.tmp"
export GOPATH="${GOPATH:-D:/WorkBuddy/Rekobox/.gopath}"
export GOCACHE="${GOCACHE:-D:/WorkBuddy/Rekobox/.gocache}"
export GOPROXY="${GOPROXY:-https://goproxy.cn,direct}"
export GOTOOLCHAIN=local GOFLAGS=-mod=mod GOOS=windows GOARCH=amd64 CGO_ENABLED=0
mkdir -p "$TMP"

PREFIX="$PWD/libs/deps/built"

#### 1. C++ 第三方依赖（zxing-cpp / yaml-cpp / protobuf） ####
if [ "${1:-}" != "--skip-deps" ]; then
    if [ ! -f "$PREFIX/bin/protoc.exe" ]; then
        echo "==> [1/5] 构建 C++ 依赖（首次约 10 分钟）"
        bash libs/build_deps_mingw.sh
    else
        echo "==> [1/5] C++ 依赖已存在，跳过"
    fi
else
    echo "==> [1/5] 跳过依赖"
fi

#### 2. Go 内核 ####
echo "==> [2/5] 构建 rekobox_core.exe"
[ -d _deps/sing-box ] || bash libs/get_source_local.sh
mkdir -p deployment/core
CORE_OUT="$PWD/deployment/core/rekobox_core.exe"
( cd go/cmd/rekobox_core && go build \
    -o "$CORE_OUT" \
    -trimpath -ldflags "-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$(cat ../../../nekoray_version.txt)" \
    -tags "with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls" )

#### 3. CMake 配置 ####
echo "==> [3/5] CMake 配置"
cmake -GNinja -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DQT_VERSION_MAJOR=6 \
    -DCMAKE_PREFIX_PATH="D:/Qt/6.7.2/mingw_64;$PREFIX" \
    -DNKR_LIBS="$PREFIX"

#### 4. 编译 ####
echo "==> [4/5] 编译"
cmake --build build -j "$(nproc 2>/dev/null || echo 8)"

#### 5. 部署 ####
echo "==> [5/5] 组装部署目录"
bash libs/deploy_windows_mingw.sh

echo
echo "产物：$PWD/deployment/windows64"
