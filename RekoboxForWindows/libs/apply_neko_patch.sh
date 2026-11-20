#!/usr/bin/env bash
# 把仓库内的 neko overlay 覆盖到 sing-box 源码树，并打上必要的补丁。
#
# 背景：go/ 下不放第三方源码，但有两类东西必须落到 sing-box 树里，否则内核编不出来/
# 或者在 mac 上跑不起来：
#   1) go/singbox-neko/          → sing-box/neko/           我们自己的包（连接记录、版本号）
#   2) go/singbox-neko-headroom/ → sing-box/neko_headroom/  UDP 回程 headroom 修补（见该包注释）
#   3) go/patches/*.patch        → 改 sing-box 既有文件（route/conn.go 调 neko_headroom）
#
# 目标树与 go/cmd/rekobox_core/go.mod 的 replace 保持一致：
#   github.com/sagernet/sing-box => ../../../../sing-box（仓库根目录，与 Android 共用）
#
# 脚本幂等：可以反复执行；补丁只在没打过时应用。
set -euo pipefail
cd "$(dirname "$0")/.."

SING_BOX_DIR="../sing-box"
if [ ! -d "$SING_BOX_DIR" ]; then
    echo "Error: $SING_BOX_DIR 不存在，先执行 libs/get_source_local.sh" >&2
    exit 1
fi

# ---------------------------------------------------------------- 1. neko 包
mkdir -p "$SING_BOX_DIR/neko"
cp -f go/singbox-neko/*.go "$SING_BOX_DIR/neko/"
echo ">> neko overlay -> $SING_BOX_DIR/neko ($(ls go/singbox-neko/*.go | wc -l) files)"

# ---------------------------------------------------------------- 2. headroom 包
mkdir -p "$SING_BOX_DIR/neko_headroom"
cp -f go/singbox-neko-headroom/*.go "$SING_BOX_DIR/neko_headroom/"
echo ">> neko_headroom -> $SING_BOX_DIR/neko_headroom ($(ls go/singbox-neko-headroom/*.go | wc -l) files)"

# ---------------------------------------------------------------- 3. 补丁
# route/conn.go: UDP 回程缺 headroom 会 panic（mac TUN 两进程模型必现）
CONN_GO="$SING_BOX_DIR/route/conn.go"
if [ ! -f "$CONN_GO" ]; then
    echo "Error: 找不到 $CONN_GO" >&2
    exit 1
fi
if grep -q "neko_headroom.Ensure" "$CONN_GO"; then
    echo ">> route/conn.go 已包含 neko_headroom，跳过补丁"
else
    echo ">> 给 route/conn.go 打 headroom 补丁"
    if ! git -C "$SING_BOX_DIR" apply --whitespace=nowarn go/patches/route-conn-headroom.patch 2>/dev/null; then
        # git apply 的相对路径是相对 sing-box 目录，上面那条在部分情况下找不到文件，回退到绝对路径
        git -C "$SING_BOX_DIR" apply --whitespace=nowarn "$(cd "$(dirname "$0")/.." && pwd)/go/patches/route-conn-headroom.patch"
    fi
    grep -q "neko_headroom.Ensure" "$CONN_GO" || { echo "Error: 补丁没生效" >&2; exit 1; }
    echo ">> route/conn.go 补丁完成"
fi
