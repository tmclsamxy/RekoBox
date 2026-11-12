#!/usr/bin/env bash
# 拉取内核三方源码。版本锁定在 libs/get_source_env.sh
#
# 布局约定（2026-11 起）：
#   ../sing-box      仓库根目录，**Windows 与 Android 共用同一棵内核源码**
#                    该树已包含 VLESS Encryption(mlkem768x25519plus) 与 Vision，
#                    Windows 端不再使用 _deps/sing-box 的旧快照
#   _deps/libneko    仅 Windows 侧使用的 libneko
#
# sing-quic 不固定为仓库内副本：交给 go.mod 解析合适版本。
set -euo pipefail
cd "$(dirname "$0")/.."

source libs/get_source_env.sh

fetch_into() { # dir repo sha
    local dir="$1" repo="$2" sha="$3"
    if [ -d "$dir" ] && [ -f "$dir/go.mod" ]; then
        echo ">> $dir 已存在"
        return
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir"
        git init -q
        git remote add origin "https://github.com/$repo.git"
        git fetch --depth 1 -q origin "$sha"
        git checkout -q FETCH_HEAD
        echo ">> $dir @ $(git rev-parse --short HEAD)"
    )
}

mkdir -p _deps

fetch_into ../sing-box  starifly/sing-box  "$COMMIT_SING_BOX"
fetch_into _deps/libneko MatsuriDayo/libneko "$COMMIT_LIBNEKO"

# 新增的公开包（cmd/sing-box 在 1.11+ 不可导入），随仓库走
bash libs/apply_neko_patch.sh
