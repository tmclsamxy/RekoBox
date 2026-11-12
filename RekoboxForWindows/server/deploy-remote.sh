#!/usr/bin/env bash
# ==============================================================================
# rekosync 远程一键部署（在开发机运行：Windows Git Bash / macOS / Linux）
#
#   本机有 Go → 交叉编译后只上传二进制，服务器不需要 Go、不需要外网装依赖
#   本机没 Go → 上传 deploy.sh，由服务器自己装依赖 + 拉源码 + 编译
#
# 用法：
#   bash deploy-remote.sh
#   bash deploy-remote.sh -h
#   HOST=1.2.3.4 ACCOUNT=alice bash deploy-remote.sh        # 部分项预填
#   bash deploy-remote.sh --force-remote-build              # 强制走服务器端编译
#
# 服务器要求：Linux、root 或免密 sudo（sudo -n）
# ==============================================================================
set -Eeuo pipefail

cd "$(dirname "$0")"

if [ -t 1 ]; then
  C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
  C_OK=''; C_WARN=''; C_ERR=''; C_DIM=''; C_END=''
fi
say()  { printf '%s==>%s %s\n' "$C_OK" "$C_END" "$*"; }
note() { printf '%s    %s%s\n' "$C_DIM" "$*" "$C_END"; }
warn() { printf '%s[!]%s %s\n' "$C_WARN" "$C_END" "$*" >&2; }
die()  { printf '%s[x]%s %s\n' "$C_ERR" "$C_END" "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
用法: bash deploy-remote.sh [选项]

  -h, --help            显示帮助
      --no-build        不在本机编译，直接用现有 dist/ 里的二进制
      --force-remote-build  忽略本机二进制，让服务器自己装 Go + 编译
      --arch ARCH       目标架构，默认 amd64（服务器是 arm 就写 arm64）

可预填环境变量: HOST / USER / PORT / SSH_KEY / REMOTE_DIR / LISTEN / ACCOUNT / PASSWORD / OPEN_FW
EOF
}

HOST="${HOST:-}"
SSH_USER="${USER:-root}"
PORT="${PORT:-22}"
SSH_KEY="${SSH_KEY:-}"
REMOTE_DIR="${REMOTE_DIR:-/opt/rekosync}"
LISTEN="${LISTEN:-:8080}"
ACCOUNT="${ACCOUNT:-alice}"
PASSWORD="${PASSWORD:-}"
OPEN_FW="${OPEN_FW:-}"
TARGET_ARCH="amd64"
DO_BUILD=1
FORCE_REMOTE=0

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --no-build) DO_BUILD=0 ;;
    --force-remote-build) FORCE_REMOTE=1 ;;
    --arch) TARGET_ARCH="${2:?缺参数}"; shift ;;
    *) die "未知参数：$1（-h 看帮助）" ;;
  esac
  shift
done

have_tty() { [ -t 0 ]; }

ask() {
  local prompt="$1" def="$2" var="$3" input=""
  if [ -n "${!var:-}" ] && [ "${!var:-}" != "$def" ]; then note "$prompt = ${!var}（预填）"; return; fi
  if ! have_tty; then printf -v "$var" '%s' "$def"; return; fi
  if [ -n "$def" ]; then
    read -r -p "$prompt [$def]: " input || true
    printf -v "$var" '%s' "${input:-$def}"
  else
    local v=""
    while [ -z "$v" ]; do read -r -p "$prompt: " v || true; done
    printf -v "$var" '%s' "$v"
  fi
}

ask_yn() {
  local prompt="$1" def="$2" var="$3" input=""
  if [ -n "${!var:-}" ]; then return; fi
  if ! have_tty; then printf -v "$var" '%s' "$def"; return; fi
  read -r -p "$prompt [$def]: " input || true
  case "${input:-$def}" in y|Y|yes|YES) printf -v "$var" '%s' "y" ;; *) printf -v "$var" '%s' "n" ;; esac
}

echo
say "RekoBox 节点同步服务 · 远程部署"
note "本机交叉编译（有 Go 时）→ 上传 → 服务器安装，或由服务器自行编译"

echo
say "服务器"
ask "服务器地址（IP 或域名）" "" HOST
ask "SSH 用户" "root" SSH_USER
ask "SSH 端口" "22" PORT
ask "SSH 私钥（留空用默认密钥/ssh-agent）" "" SSH_KEY

echo
say "安装参数"
ask "安装目录" "/opt/rekosync" REMOTE_DIR
ask "监听地址（:8080 对外 / 127.0.0.1:8080 走 nginx）" ":8080" LISTEN
ask "同步账号名（输入 - 跳过建号）" "alice" ACCOUNT
if [ "$ACCOUNT" = "-" ]; then ACCOUNT=""; fi

GEN_PASS=0
if [ -n "$ACCOUNT" ] && [ -z "$PASSWORD" ] && have_tty; then
  read -r -s -p "账号口令（至少 6 位，直接回车自动生成）: " PASSWORD || true
  echo
fi
if [ -n "$ACCOUNT" ] && [ -z "$PASSWORD" ]; then
  PASSWORD="$(LC_ALL=C tr -dc 'A-Za-z2-9' </dev/urandom | head -c 20)"; GEN_PASS=1
fi
if [ -n "$ACCOUNT" ] && [ "${#PASSWORD}" -lt 6 ]; then die "口令至少 6 位"; fi

ask_yn "放行服务器防火墙端口" "y" OPEN_FW

# ---------- 本地二进制 ----------
BIN=""
if [ "$FORCE_REMOTE" = 0 ]; then
  BIN="dist/rekosync-linux-${TARGET_ARCH}"
  if [ "$DO_BUILD" = 1 ]; then
    if command -v go >/dev/null 2>&1 || [ -x /d/rea-tools/go/bin/go ]; then
      say "交叉编译 linux/${TARGET_ARCH}"
      ARCH="$TARGET_ARCH" bash ../libs/build_linux_sync.sh "$TARGET_ARCH" >/dev/null || warn "本机编译失败"
    else
      warn "本机没有 Go，跳过本机编译"
    fi
  fi
  [ -f "$BIN" ] || BIN=""
fi

if [ -n "$BIN" ]; then
  MODE="binary"
  note "将上传预编译二进制：$BIN（$(du -h "$BIN" | cut -f1)）"
  note "服务器无需 Go、无需外网"
else
  MODE="bootstrap"
  note "将上传安装脚本，由服务器自行安装 Go + 拉源码 + 编译"
  note "（服务器需要能访问外网；墙内可用 GOPROXY=goproxy.cn）"
fi

cat <<EOF

──────────── 即将执行 ────────────
  目标        $SSH_USER@$HOST:$PORT
  安装目录    $REMOTE_DIR
  监听        $LISTEN
  同步账号    ${ACCOUNT:-（跳过）}
  安装方式    $([ "$MODE" = "binary" ] && echo "上传预编译二进制" || echo "服务器端编译")
----------------------------------
EOF
ask_yn "确认开始" "y" CONFIRM
[ "$CONFIRM" = "y" ] || { say "已取消"; exit 0; }

command -v ssh >/dev/null 2>&1 || die "缺少 ssh"
command -v scp >/dev/null 2>&1 || die "缺少 scp"

SSH_OPTS=(-p "$PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)
if [ -n "$SSH_KEY" ]; then SSH_OPTS+=(-i "$SSH_KEY"); fi
SSH=(ssh "${SSH_OPTS[@]}")
SCP=(scp "${SSH_OPTS[@]}")

say "检查 SSH"
"${SSH[@]}" "$SSH_USER@$HOST" "echo ok" >/dev/null || die "SSH 连不上：$SSH_USER@$HOST:$PORT"

PRIV=""
if "${SSH[@]}" "$SSH_USER@$HOST" "id -u" 2>/dev/null | grep -qx 0; then
  PRIV=""; note "登录用户是 root"
else
  "${SSH[@]}" "$SSH_USER@$HOST" "sudo -n true" >/dev/null 2>&1 || die "该用户不是 root，也没有免密 sudo"
  PRIV="sudo -n"; note "使用 sudo -n"
fi

REMOTE_SCRIPT="/tmp/rekosync-install.$$"
say "上传安装脚本"
"${SCP[@]}" deploy.sh "$SSH_USER@$HOST:$REMOTE_SCRIPT" || die "上传 deploy.sh 失败"

REMOTE_ARGS=(-y --listen "$LISTEN" --dir "$REMOTE_DIR")
if [ -n "$ACCOUNT" ]; then REMOTE_ARGS+=(--account "$ACCOUNT" --password -); else REMOTE_ARGS+=(--no-account); fi
if [ "$OPEN_FW" = "y" ]; then REMOTE_ARGS+=(--firewall); else REMOTE_ARGS+=(--no-firewall); fi

if [ "$MODE" = "binary" ]; then
  REMOTE_BIN="/tmp/rekosync.bin.$$"
  say "上传二进制（约 $(du -h "$BIN" | cut -f1)）"
  "${SCP[@]}" "$BIN" "$SSH_USER@$HOST:$REMOTE_BIN" || die "上传二进制失败"
  REMOTE_ARGS+=(--binary "$REMOTE_BIN")
fi

# 口令通过 stdin 传给 --password -，不出现在命令行/ps 里
say "开始安装（日志实时输出）"
set +e
if [ -n "$ACCOUNT" ]; then
  printf '%s\n' "$PASSWORD" | "${SSH[@]}" "$SSH_USER@$HOST" "$PRIV bash $REMOTE_SCRIPT ${REMOTE_ARGS[*]}"
else
  "${SSH[@]}" "$SSH_USER@$HOST" "$PRIV bash $REMOTE_SCRIPT ${REMOTE_ARGS[*]}" </dev/null
fi
RC=$?
set -e

"${SSH[@]}" "$SSH_USER@$HOST" "rm -f $REMOTE_SCRIPT ${REMOTE_BIN:-}" >/dev/null 2>&1 || true

if [ "$RC" -ne 0 ]; then
  die "远程安装失败（退出码 $RC）。排查：ssh $SSH_USER@$HOST 'journalctl -u rekosync -n 50 --no-pager'"
fi

echo
say "完成"
note "服务端：systemctl status rekosync --no-pager"
note "日志：  journalctl -u rekosync -f"
if [ -n "$ACCOUNT" ] && [ "$GEN_PASS" = 1 ]; then
  printf '  账号 %s  口令 %s%s%s（只显示这一次）\n' "$ACCOUNT" "$C_OK" "$PASSWORD" "$C_END"
fi
