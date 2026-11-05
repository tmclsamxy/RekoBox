#!/usr/bin/env bash
# 把仓库内的 neko overlay 覆盖到 sing-box 源码树。
#
# 背景：_deps/ 不入库（体积大、可重新拉取），但 go/singbox-neko/ 是我们对
# sing-box 唯一的新增包（1.11+ 的 cmd/sing-box 不能导入，需要公开的 Create/QueryStats）。
# 不 overlay 的话，重新拉源码后内核会编译失败。
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -d _deps/sing-box ]; then
    echo "Error: _deps/sing-box 不存在，先执行 libs/get_source_local.sh" >&2
    exit 1
fi

mkdir -p _deps/sing-box/neko
cp -f go/singbox-neko/*.go _deps/sing-box/neko/
echo ">> neko overlay -> _deps/sing-box/neko ($(ls go/singbox-neko/*.go | wc -l) files)"
