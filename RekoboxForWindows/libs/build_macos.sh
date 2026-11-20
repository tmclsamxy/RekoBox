#!/usr/bin/env bash
# Rekobox for macOS —— 一键构建（Apple Silicon / M2 起）
#
#   bash libs/build_macos.sh              # 编译 → .app（ad-hoc 签名）
#   bash libs/build_macos.sh --dmg        # 再打一个 .dmg
#   bash libs/build_macos.sh --qt ~/Qt/6.7.2/macos
#
# 前置：
#   xcode-select --install
#   brew install cmake ninja go
#   python3 -m aqt install-qt mac desktop 6.7.2 clang_64 -O ~/Qt --archives qtbase qtsvg qttools
#   bash libs/build_deps_macos.sh        （zxing 2.0 / yaml 0.7 / protobuf 21.4，与 Windows 对齐）
#   bash libs/get_source_local.sh        （拉 sing-box 内核源码）
#
# 说明：
#   - 源码根目录是 RekoboxForWindows/（桌面端共享树），构建产物落在 ../RekoboxForMac/build
#   - 内核 rekobox_core 从 go/cmd/rekobox_core 编（darwin/arm64）；已有产物则跳过
#   - 默认 ad-hoc 签名（本机可用，别人下载要右键打开）；正式分发见 docs/06-macos-port.md §7

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"          # RekoboxForWindows/
REPO="$(cd "$ROOT/.." && pwd)"                                    # 仓库根
OUT="$REPO/RekoboxForMac/build"
DIST="$REPO/RekoboxForMac/dist"
QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.7.2/macos}"
SINGBOX_SRC="${SINGBOX_SRC:-$REPO/sing-box}"
CORE_TAGS="with_clash_api,with_v2ray_api,with_gvisor,with_quic,with_wireguard,with_utls"
DO_DMG=0
DO_CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --qt)    QT_PREFIX="$2"; shift 2 ;;
        --dmg)   DO_DMG=1; shift ;;
        --clean) DO_CLEAN=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数: $1"; exit 2 ;;
    esac
done

die() { echo "错误: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

[ "$(uname -s)" = "Darwin" ] || die "这个脚本只能在 macOS 上跑"
for c in cmake ninja go codesign; do
    command -v "$c" >/dev/null 2>&1 || die "缺少 $c（xcode-select --install / brew install $c）"
done
[ -x "$QT_PREFIX/bin/qmake" ] || die "Qt 不在 $QT_PREFIX，用 --qt 指定，或先 aqt install-qt mac desktop 6.7.2 clang_64"
[ -x "$QT_PREFIX/bin/macdeployqt" ] || die "找不到 macdeployqt，Qt 组件不全（需要 qtbase qtsvg qttools）"

NKR_VERSION="$(tr -d '\r\n' < "$ROOT/nekoray_version.txt")"
echo "Rekobox $NKR_VERSION · Qt $("$QT_PREFIX/bin/qmake" -query QT_VERSION) · $(uname -m)"

# ---------------------------------------------------------------- 1. 图标
if [ ! -f "$ROOT/res/rekobox.icns" ]; then
    step "生成 res/rekobox.icns"
    bash "$ROOT/libs/make_icns.sh"
fi

# ---------------------------------------------------------------- 2. 内核
# 内核从 go/cmd/rekobox_core 构建（它 import libneko + sing-box/neko），
# 前提是仓库根目录有 sing-box 源码树，且已 overlay neko 包：
#   bash libs/get_source_local.sh && bash libs/apply_neko_patch.sh
step "编译内核 rekobox_core（darwin/arm64）"
mkdir -p "$OUT/core"
if [ -d "$SINGBOX_SRC" ]; then
    # 每次都比对一次 overlay + 补丁（脚本幂等）：neko 包没了要补，route/conn.go 的
    # headroom 补丁没打过也要补——后者不打，mac TUN 的 UDP 回程会直接 panic
    bash "$ROOT/libs/apply_neko_patch.sh" || die "apply_neko_patch.sh 失败"
    ( cd "$ROOT/go/cmd/rekobox_core" \
      && CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 \
         go build -trimpath \
         -ldflags "-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$NKR_VERSION" \
         -tags "$CORE_TAGS" \
         -o "$OUT/core/rekobox_core" . )
    echo "内核: $OUT/core/rekobox_core"
    "$OUT/core/rekobox_core" version 2>/dev/null | head -2 || true
elif [ -f "$OUT/core/rekobox_core" ]; then
    echo "没找到 sing-box 源码树，沿用已有的 $OUT/core/rekobox_core"
else
    die "既没有 $SINGBOX_SRC，也没有 $OUT/core/rekobox_core。先跑 libs/get_source_local.sh 拉内核源码"
fi

# ---------------------------------------------------------------- 3. configure
step "configure"
if [ "$DO_CLEAN" = "1" ]; then
    rm -rf "$OUT/CMakeCache.txt" "$OUT/CMakeFiles" "$OUT/build.ninja"
fi

PREFIX_PATH="$QT_PREFIX"
if [ -n "${NKR_LIBS:-}" ] && [ -d "${NKR_LIBS}" ]; then
    # 推荐：先用 libs/build_deps_macos.sh 把 21.4 / 0.7.0 / 2.0.0 编好
    PREFIX_PATH="$PREFIX_PATH;$NKR_LIBS"
    echo "依赖目录: $NKR_LIBS"
elif [ -d "$ROOT/libs/deps/built" ]; then
    PREFIX_PATH="$PREFIX_PATH;$ROOT/libs/deps/built"
    echo "依赖目录: $ROOT/libs/deps/built（源码构建）"
elif command -v brew >/dev/null 2>&1; then
    # 兜底：用 Homebrew 的版本，可能与 Windows 端不一致（见 build_deps_macos.sh 的说明）
    for p in yaml-cpp zxing-cpp protobuf; do
        if bp="$(brew --prefix "$p" 2>/dev/null)" && [ -d "$bp" ]; then
            PREFIX_PATH="$PREFIX_PATH;$bp"
        else
            die "缺少依赖 $p（brew install $p），或先跑 libs/build_deps_macos.sh"
        fi
    done
    echo "依赖目录: Homebrew（注意 protobuf 版本可能比 Windows 端新）"
else
    die "没有 Homebrew，也没有 NKR_LIBS。先跑 libs/build_deps_macos.sh"
fi
echo "CMAKE_PREFIX_PATH=$PREFIX_PATH"

cmake -G Ninja \
      -S "$ROOT" -B "$OUT" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$PREFIX_PATH"

# ---------------------------------------------------------------- 4. build
step "编译"
ninja -C "$OUT"

APP="$OUT/RekoBox.app"
[ -d "$APP" ] || die "没有产出 $APP（检查 OUTPUT_NAME / MACOSX_BUNDLE 配置）"

# ---------------------------------------------------------------- 5. 部署
step "放入内核并部署 Qt 运行时"
cp "$OUT/core/rekobox_core" "$APP/Contents/MacOS/rekobox_core"
chmod +x "$APP/Contents/MacOS/rekobox_core"
# CMake 只在有资源要装时才建 Resources；没有 NSMainNib 的 bundle 里它不存在，
# 直接 cp 图标会得到 "No such file or directory (os error 2)"
mkdir -p "$APP/Contents/Resources"
if [ -f "$ROOT/res/rekobox.icns" ]; then
    cp "$ROOT/res/rekobox.icns" "$APP/Contents/Resources/rekobox.icns"
    echo "图标: $APP/Contents/Resources/rekobox.icns"
fi
"$QT_PREFIX/bin/macdeployqt" "$APP" -always-overwrite

# Qt 自带翻译（qtbase_*.qm）：macdeployqt 不拷，代码从
# Contents/Resources/translations 里找。没有 qtbase 归档就跳过，界面仍是中文源串。
QT_TR_SRC="$QT_PREFIX/translations"
if [ -d "$QT_TR_SRC" ]; then
    mkdir -p "$APP/Contents/Resources/translations"
    cp "$QT_TR_SRC"/qtbase_*.qm "$APP/Contents/Resources/translations/" 2>/dev/null || true
    echo "Qt 翻译: $(ls "$APP/Contents/Resources/translations" | wc -l | tr -d ' ') 个"
else
    echo "跳过 Qt 翻译：$QT_TR_SRC 不存在（aqt 需 --archives qttranslations）"
fi

# ---------------------------------------------------------------- 6. 签名（ad-hoc）
step "ad-hoc 签名"
# 早期构建会在 bundle 里留一个断链的 updater -> launcher（Linux 才需要），
# 断链符号链接会让 codesign --deep 校验失败（真机报"已损坏"），每次构建清掉
rm -f "$APP/Contents/MacOS/updater"
codesign --force --sign - "$APP/Contents/MacOS/rekobox_core"
codesign --force --deep --sign - "$APP"
codesign -dv --verbose=2 "$APP" 2>&1 | head -5 || true

# ---------------------------------------------------------------- 7. dmg
if [ "$DO_DMG" = "1" ]; then
    step "打包 dmg"
    mkdir -p "$DIST"
    DMG="$DIST/Rekobox-$NKR_VERSION-mac-arm64.dmg"
    rm -f "$DMG"

    # 铺一个临时目录：app + /Applications 快捷方式，用户拖一下就装好。
    # 用 ditto 而不是 cp -R：后者会丢扩展属性，签名跟着一起坏。
    STAGE="$(mktemp -d "${TMPDIR:-/tmp}/rekobox-dmg.XXXXXX")"
    trap 'rm -rf "$STAGE"' EXIT
    ditto "$APP" "$STAGE/RekoBox.app"
    ln -s /Applications "$STAGE/Applications"

    hdiutil create -volname "RekoBox" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
    echo "产物: $DMG ($(du -h "$DMG" | cut -f1))"
fi

echo
echo "完成: $APP"
echo "打开: open \"$APP\""
echo "配置/日志: ~/Library/Application Support/rekobox"
