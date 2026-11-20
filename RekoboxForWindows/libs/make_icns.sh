#!/usr/bin/env bash
# 从 res/public/rekobox.png 生成 res/rekobox.icns（只能在 macOS 上跑）
#
#   bash libs/make_icns.sh [源 png] [输出 icns]
#
# 源图目前是 256×256，够用但 512/1024 档是放大的；以后有 1024 的图直接替换即可。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:-$ROOT/res/public/rekobox.png}"
OUT="${2:-$ROOT/res/rekobox.icns}"

command -v iconutil >/dev/null 2>&1 || { echo "iconutil 不存在（需要 macOS）" >&2; exit 1; }
[ -f "$SRC" ] || { echo "找不到源图: $SRC" >&2; exit 1; }

TMP="$(mktemp -d)"
ICONSET="$TMP/rekobox.iconset"
mkdir -p "$ICONSET"

for s in 16 32 128 256 512; do
    sips -z "$s" "$s" "$SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z "$((s * 2))" "$((s * 2))" "$SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done

iconutil -c icns "$ICONSET" -o "$OUT"
rm -rf "$TMP"
echo "已生成: $OUT"
