#!/usr/bin/env bash
# 拉取内核三方源码到本仓库 _deps/（替代上游 libs/get_source.sh 的仓库外 ../sing-box 布局）
# 版本锁定在 libs/get_source_env.sh
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

fetch sing-box  MatsuriDayo/sing-box  "$COMMIT_SING_BOX"
fetch sing-quic MatsuriDayo/sing-quic "$COMMIT_SING_QUIC"
fetch libneko   MatsuriDayo/libneko   "$COMMIT_LIBNEKO"
