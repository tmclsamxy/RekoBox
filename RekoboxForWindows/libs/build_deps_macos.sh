#!/usr/bin/env bash
# 从源码构建 nekoray 的三个 C++ 依赖（macOS / arm64），版本与 Windows 端对齐：
#   zxing-cpp 2.0.0 / yaml-cpp 0.7.0 / protobuf 21.4
#
# 为什么不用 brew：
#   brew 的 protobuf 已经是 v2x/v3x（依赖 abseil），生成的 .pb.cc 与工程里手写的
#   gRPC-over-HTTP2 客户端不一定兼容；yaml-cpp / zxing-cpp 也有 API 漂移。
#   这里锁定与 Windows 完全相同的版本，两边行为一致，出问题只需查一套。
#
#   bash libs/build_deps_macos.sh
#   NKR_LIBS="$PWD/libs/deps/built" bash libs/build_macos.sh
#
# 产物：libs/deps/built（静态库 + CMake config）

set -euo pipefail
cd "$(dirname "$0")/.."

[ "$(uname -s)" = "Darwin" ] || { echo "只能在 macOS 上跑" >&2; exit 1; }

DEPS="$PWD/libs/deps"
PREFIX="$DEPS/built"
mkdir -p "$DEPS"
cd "$DEPS"

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG="$DEPS/build.log"
: > "$LOG"

log_tail() { echo "---- 最后 30 行 ($1) ----"; tail -30 "$LOG"; }

#### zxing-cpp 2.0.0 ####
if [ ! -d zxing-cpp-2.0.0 ]; then
    echo ">> 下载 zxing-cpp"
    curl -fL -o zxing.zip https://github.com/nu-book/zxing-cpp/archive/refs/tags/v2.0.0.zip
    unzip -q zxing.zip && rm zxing.zip
fi
echo ">> zxing-cpp"
cmake -S zxing-cpp-2.0.0 -B zxing-cpp-2.0.0/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_EXAMPLES=OFF -DBUILD_BLACKBOX_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >> "$LOG" 2>&1 || { log_tail zxing; exit 1; }
cmake --build zxing-cpp-2.0.0/build -j "$JOBS" >> "$LOG" 2>&1 &&
cmake --install zxing-cpp-2.0.0/build >> "$LOG" 2>&1 || { log_tail zxing; exit 1; }

#### yaml-cpp 0.7.0 ####
if [ ! -d yaml-cpp-yaml-cpp-0.7.0 ]; then
    echo ">> 下载 yaml-cpp"
    curl -fL -o yaml.zip https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.7.0.zip
    unzip -q yaml.zip && rm yaml.zip
fi
echo ">> yaml-cpp"
cmake -S yaml-cpp-yaml-cpp-0.7.0 -B yaml-cpp-yaml-cpp-0.7.0/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >> "$LOG" 2>&1 || { log_tail yaml; exit 1; }
cmake --build yaml-cpp-yaml-cpp-0.7.0/build -j "$JOBS" >> "$LOG" 2>&1 &&
cmake --install yaml-cpp-yaml-cpp-0.7.0/build >> "$LOG" 2>&1 || { log_tail yaml; exit 1; }

#### protobuf 21.4 ####
if [ ! -d protobuf ]; then
    echo ">> 下载 protobuf 21.4"
    git clone -q --recurse-submodules -b v21.4 --depth 1 --shallow-submodules \
        https://github.com/protocolbuffers/protobuf
fi
echo ">> protobuf 21.4（比较慢，约 5-10 分钟）"
cmake -S protobuf -B protobuf/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_EXAMPLES=OFF \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >> "$LOG" 2>&1 || { log_tail protobuf; exit 1; }
cmake --build protobuf/build -j "$JOBS" >> "$LOG" 2>&1 &&
cmake --install protobuf/build >> "$LOG" 2>&1 || { log_tail protobuf; exit 1; }

echo
echo ">> 完成: $PREFIX"
echo ">> 下一步: NKR_LIBS=\"$PREFIX\" bash libs/build_macos.sh"
