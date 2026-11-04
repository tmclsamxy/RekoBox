#!/usr/bin/env bash
# 用 MinGW + Ninja 构建 nekoray 的 C++ 第三方依赖（Windows 原生，无需 MSVC）
# 产物安装到 libs/deps/built，供 CMake 的 NKR_LIBS 使用
set -euo pipefail
cd "$(dirname "$0")/.."

export PATH="/d/Qt/Tools/mingw1310_64/bin:/c/Users/Mrcheng/AppData/Local/Programs/Python/Python310/Scripts:$PATH"
export CC=gcc CXX=g++
export TMP="D:/WorkBuddy/Rekobox/.tmp" TEMP="D:/WorkBuddy/Rekobox/.tmp"
mkdir -p "$TMPDIR" 2>/dev/null || true

DEPS="$PWD/libs/deps"
PREFIX="$DEPS/built"
mkdir -p "$DEPS"
cd "$DEPS"
rm -rf "$PREFIX"
mkdir -p "$PREFIX"

JOBS=$(nproc 2>/dev/null || echo 8)

#### zxing-cpp ####
if [ ! -d zxing-* ]; then
  echo ">> zxing-cpp"
  curl -fL -o dl.zip https://github.com/nu-book/zxing-cpp/archive/refs/tags/v2.0.0.zip
  unzip -q dl.zip && rm dl.zip
fi
cd zxing-*/
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_BLACKBOX_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" > /tmp/dep-zxing.log 2>&1 \
  || { tail -30 /tmp/dep-zxing.log; exit 1; }
cmake --build build -j "$JOBS" >> /tmp/dep-zxing.log 2>&1 && cmake --install build >> /tmp/dep-zxing.log 2>&1 \
  || { tail -30 /tmp/dep-zxing.log; exit 1; }
cd ..

#### yaml-cpp ####
if [ ! -d yaml-cpp-* ]; then
  echo ">> yaml-cpp"
  curl -fL -o dl.zip https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.7.0.zip
  unzip -q dl.zip && rm dl.zip
fi
cd yaml-cpp-*/
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TESTING=OFF -DYAML_CPP_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" > /tmp/dep-yaml.log 2>&1 \
  || { tail -30 /tmp/dep-yaml.log; exit 1; }
cmake --build build -j "$JOBS" >> /tmp/dep-yaml.log 2>&1 && cmake --install build >> /tmp/dep-yaml.log 2>&1 \
  || { tail -30 /tmp/dep-yaml.log; exit 1; }
cd ..

#### protobuf ####
if [ ! -d protobuf ]; then
  echo ">> protobuf 21.4"
  git clone -q --recurse-submodules -b v21.4 --depth 1 --shallow-submodules \
    https://github.com/protocolbuffers/protobuf
fi
cmake -S protobuf -B protobuf/build -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" > /tmp/dep-pb.log 2>&1 \
  || { tail -30 /tmp/dep-pb.log; exit 1; }
cmake --build protobuf/build -j "$JOBS" >> /tmp/dep-pb.log 2>&1 && cmake --install protobuf/build >> /tmp/dep-pb.log 2>&1 \
  || { tail -30 /tmp/dep-pb.log; exit 1; }

echo ">> done: $PREFIX"
ls "$PREFIX"
