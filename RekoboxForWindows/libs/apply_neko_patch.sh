#!/usr/bin/env bash
# 把仓库内的 neko overlay 覆盖到 sing-box 源码树。
#
# 背景：go/ 下不放第三方源码，但 go/singbox-neko/ 是我们对 sing-box 唯一的新增包
# （1.11+ 的 cmd/sing-box 不能导入，需要公开的 Create/QueryStats）。
# 不 overlay 的话，重新拉源码后内核会编译失败。
#
# 目标树与 go/cmd/rekobox_core/go.mod 的 replace 保持一致：
#   github.com/sagernet/sing-box => ../../../../sing-box（仓库根目录，与 Android 共用）
set -euo pipefail
cd "$(dirname "$0")/.."

SING_BOX_DIR="../sing-box"
if [ ! -d "$SING_BOX_DIR" ]; then
    echo "Error: $SING_BOX_DIR 不存在，先执行 libs/get_source_local.sh" >&2
    exit 1
fi

mkdir -p "$SING_BOX_DIR/neko"
cp -f go/singbox-neko/*.go "$SING_BOX_DIR/neko/"
echo ">> neko overlay -> $SING_BOX_DIR/neko ($(ls go/singbox-neko/*.go | wc -l) files)"
