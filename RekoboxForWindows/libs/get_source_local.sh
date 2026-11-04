#!/usr/bin/env bash
# 拉取内核三方源码到本仓库 _deps/（替代上游 libs/get_source.sh 的仓库外 ../sing-box 布局）
# 版本锁定在 libs/get_source_env.sh
#
# sing-quic 不再固定为仓库内副本：sing-box 1.12 需要配套的 quic-go 接口，
# 交给 go.mod 解析合适版本。
set -euo pipefail
cd "$(dirname "$0")/.."

source libs/get_source_env.sh

mkdir -p _deps
cd _deps

fetch() { # name repo sha
    local name="$1" repo="$2" sha="$3"
    if [ -d "$name" ] && [ -f "$name/go.mod" ]; then
        echo ">> $name 已存在"
        return
    fi
    rm -rf "$name"
    mkdir "$name"
    (
        cd "$name"
        git init -q
        git remote add origin "https://github.com/$repo.git"
        git fetch --depth 1 -q origin "$sha"
        git checkout -q FETCH_HEAD
        echo ">> $name @ $(git rev-parse --short HEAD)"
    )
}

fetch sing-box MatsuriDayo/sing-box "$COMMIT_SING_BOX"
fetch libneko  MatsuriDayo/libneko  "$COMMIT_LIBNEKO"
