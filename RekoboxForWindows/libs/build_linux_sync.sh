#!/usr/bin/env bash
# 交叉编译 Linux 版 rekosync（在 Windows 开发机上跑，产物直接 scp 到服务器）
#
# 用法：
#   bash libs/build_linux_sync.sh             # 生成 server/dist/rekosync-linux-amd64
#   bash libs/build_linux_sync.sh arm64       # 生成 arm64 版
set -euo pipefail
cd "$(dirname "$0")/.."

ARCH="${1:-amd64}"
export PATH="/d/rea-tools/go/bin:$PATH"
export CGO_ENABLED=0
export GOOS=linux
export GOARCH="$ARCH"
export GOFLAGS=-mod=mod
export GOPROXY="${GOPROXY:-https://goproxy.cn,direct}"

mkdir -p server/dist
OUT="server/dist/rekosync-linux-${ARCH}"
go build -C server -trimpath -ldflags "-s -w" -o "$(basename "$OUT")" .
mv "server/$(basename "$OUT")" "$OUT"
ls -lh "$OUT"
echo "==> 上传：scp $OUT root@<server>:/opt/rekosync/rekosync"
