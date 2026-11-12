#!/usr/bin/env bash
# ============================================================================
# rekosync 一键部署（交互式）
#
#   在开发机（Windows Git Bash / macOS / Linux）执行，通过 SSH 部署到自有服务器：
#   上传二进制 → 建系统用户 → 装 systemd 服务 → 建同步账号 → 自检 → 打印客户端配置
#
# 用法：
#   bash deploy.sh                    # 全交互，回车用默认值
#   bash deploy.sh -h                 # 看参数说明
#   HOST=1.2.3.4 ACCOUNT=alice bash deploy.sh      # 部分项用环境变量预填
#
# 依赖：ssh、scp（Git Bash 自带）；本地无现成二进制时用 go 交叉编译。
# 服务器要求：root 或可 `sudo -n`（免密 sudo）；Linux x86_64/arm64。
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")"

C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
say()  { printf '%s==>%s %s\n' "$C_OK" "$C_END" "$*"; }
warn() { printf '%s[!]%s %s\n' "$C_WARN" "$C_END" "$*"; }
die()  { printf '%s[x]%s %s\n' "$C_ERR" "$C_END" "$*" >&2; exit 1; }
note() { printf '%s    %s%s\n' "$C_DIM" "$*" "$C_END"; }

usage() {
  cat <<'EOF'
用法: bash deploy.sh [选项]

选项:
  -h, --help        显示本帮助
      --local       不部署到远端，只在本地编译出 Linux 二进制
      --no-build    不编译，直接使用 server/dist/rekosync-linux-<arch>

可预填的环境变量（不填则交互询问）:
  HOST       服务器地址（IP 或域名）
  USER       SSH 用户，默认 root
  PORT       SSH 端口，默认 22
  SSH_KEY   私钥路径（可选，默认用 ssh-agent / 默认密钥）
  REMOTE_DIR 安装目录，默认 /opt/rekosync
  LISTEN     监听地址，默认 :8080
  ACCOUNT    同步账号名（留空则跳过建号）
  PASSWORD   同步账号口令（留空则自动生成随机口令）
  OPEN_FW    是否放行防火墙端口，y/n
EOF
}

# ---------- 交互小工具 ----------

ask() { # ask <提示> <默认值> <变量名>
  local prompt="$1" def="$2" var="$3" input=""
  if [ -n "${!var:-}" ]; then
    note "$prompt = ${!var}（来自环境变量）"
    return
  fi
  if [ -n "$def" ]; then
    read -r -p "$prompt [$def]: " input || true
    printf -v "$var" '%s' "${input:-$def}"
  else
    while [ -z "$input" ]; do
      read -r -p "$prompt: " input || true
      if [ -z "$input" ]; then warn "该项必填"; fi
    done
    printf -v "$var" '%s' "$input"
  fi
}

ask_opt() { # ask_opt <提示> <默认值> <变量名>  —— 允许留空
  local prompt="$1" def="$2" var="$3" input=""
  if [ -n "${!var:-}" ]; then
    note "$prompt = ${!var}（来自环境变量）"
    return
  fi
  if [ -n "$def" ]; then
    read -r -p "$prompt [$def]: " input || true
    printf -v "$var" '%s' "${input:-$def}"
  else
    read -r -p "$prompt: " input || true
    printf -v "$var" '%s' "$input"
  fi
}

ask_yn() { # ask_yn <提示> <默认 y/n> <变量名>
  local prompt="$1" def="$2" var="$3" input=""
  if [ -n "${!var:-}" ]; then return; fi
  read -r -p "$prompt [$def]: " input || true
  input="${input:-$def}"
  case "$input" in
    y|Y|yes|YES) printf -v "$var" '%s' "y" ;;
    *)           printf -v "$var" '%s' "n" ;;
  esac
}

rand_pass() {
  # 20 位、去掉易混淆字符（0/O/1/l），够用且好抄
  local p
  p=$(LC_ALL=C tr -dc 'A-Za-z2-9' </dev/urandom 2>/dev/null | head -c 20) || true
  if [ -n "$p" ]; then printf '%s' "$p"; else printf 'Reko-%s' "$(date +%s)"; fi
}

need() { command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1"; }

# ---------- 参数 ----------

LOCAL_ONLY=n
NO_BUILD=n
for arg in "$@"; do
  case "$arg" in
    -h|--help) usage; exit 0 ;;
    --local)   LOCAL_ONLY=y ;;
    --no-build) NO_BUILD=y ;;
    *) die "未知参数：$arg（-h 查看帮助）" ;;
  esac
done

echo
say "RekoBox 节点同步服务 —— 部署向导"
note "回车使用方括号内的默认值；口令不会回显，也不会留在服务器历史里。"
echo

# ---------- 1. 准备二进制 ----------

ARCH="${ARCH:-amd64}"
BIN="dist/rekosync-linux-${ARCH}"

if [ "$NO_BUILD" = "n" ]; then
  if command -v go >/dev/null 2>&1 || [ -x /d/rea-tools/go/bin/go ]; then
    say "交叉编译服务端（linux/${ARCH}）"
    ARCH="$ARCH" bash ../libs/build_linux_sync.sh "$ARCH"
  else
    warn "本机没有 go，跳过编译"
  fi
fi

if [ ! -f "$BIN" ] && [ -f "${BIN}.exe" ]; then BIN="${BIN}.exe"; fi
[ -f "$BIN" ] || die "找不到 $BIN。请先在有 Go 的机器上执行 ../libs/build_linux_sync.sh，或加 --no-build 前先放好产物"

say "使用二进制：$BIN（$(du -h "$BIN" | cut -f1)）"

if [ "$LOCAL_ONLY" = "y" ]; then
  say "本地模式结束。产物：$PWD/$BIN"
  exit 0
fi

# ---------- 2. 采集部署参数 ----------

echo
say "服务器信息"
ask "服务器地址（IP 或域名）" "" HOST
ask "SSH 用户" "root" USER
ask "SSH 端口" "22" PORT
ask_opt "SSH 私钥路径（留空用默认密钥/ssh-agent）" "" SSH_KEY

echo
say "安装参数"
ask "安装目录" "/opt/rekosync" REMOTE_DIR
ask "监听地址（0.0.0.0:8080 对外，127.0.0.1:8080 交给 nginx 反代）" ":8080" LISTEN

echo
say "同步账号（本程序不提供注册，账号只能这样建）"
ask_opt "账号名（小写字母/数字，2-32 位；输入 - 跳过建号）" "alice" ACCOUNT
if [ "$ACCOUNT" = "-" ]; then ACCOUNT=""; fi
if [ -n "$ACCOUNT" ] && [ -z "${PASSWORD:-}" ]; then
  read -r -s -p "账号口令（至少 6 位；直接回车自动生成）: " PASSWORD || true
  echo
fi
GEN_PASS=n
if [ -n "$ACCOUNT" ] && [ -z "${PASSWORD:-}" ]; then
  PASSWORD="$(rand_pass)"; GEN_PASS=y
fi
if [ -n "$ACCOUNT" ] && [ "${#PASSWORD}" -lt 6 ]; then
  die "口令至少 6 位"
fi
ask_yn "放行防火墙端口（ufw/firewalld，需要时选 y）" "n" OPEN_FW

# ---------- 3. 确认 ----------

cat <<EOF

──────────── 即将执行 ────────────
  目标        $USER@$HOST:$PORT
  安装目录    $REMOTE_DIR
  监听        $LISTEN
  系统服务    rekosync.service（开机自启 + 崩溃自动拉起）
  同步账号    ${ACCOUNT:-（跳过）}
  防火墙      $([ "$OPEN_FW" = "y" ] && echo "放行监听端口" || echo "不改动")
----------------------------------
EOF
ask_yn "确认开始部署" "y" CONFIRM
[ "$CONFIRM" = "y" ] || { say "已取消，未做任何改动"; exit 0; }

# ---------- 4. 连通性 ----------

need ssh
need scp

SSH_OPTS=(-p "$PORT" -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)
if [ -n "${SSH_KEY:-}" ]; then SSH_OPTS+=(-i "$SSH_KEY"); fi
SSH=(ssh "${SSH_OPTS[@]}")
SCP=(scp "${SSH_OPTS[@]}")

say "检查 SSH 连通性"
"${SSH[@]}" "$USER@$HOST" "echo ok" >/dev/null || die "SSH 连不上：$USER@$HOST:$PORT"

PRIV=""
if "${SSH[@]}" "$USER@$HOST" "id -u" 2>/dev/null | grep -qx 0; then
  PRIV=""
  note "登录用户是 root，直接执行"
elif "${SSH[@]}" "$USER@$HOST" "sudo -n true" >/dev/null 2>&1; then
  PRIV="sudo -n"
  note "检测到免密 sudo"
else
  die "该用户既不是 root 也没有免密 sudo。请用 root 登录，或给该用户配置 NOPASSWD"
fi

# ---------- 5. 上传 ----------

TMP_BIN="/tmp/rekosync.deploy.$$"
say "上传二进制到 $TMP_BIN"
"${SCP[@]}" "$BIN" "$USER@$HOST:$TMP_BIN" || die "上传失败"

# ---------- 6. 远端安装 ----------

say "远端安装与配置"

# 注意：整段脚本通过 stdin 送到远端 bash，口令不进 ps、不落盘
"${SSH[@]}" "$USER@$HOST" "$PRIV bash -s" <<REMOTE
set -e

REMOTE_DIR="$REMOTE_DIR"
LISTEN="$LISTEN"
ACCOUNT="$ACCOUNT"
PASSWORD="$PASSWORD"
OPEN_FW="$OPEN_FW"
TMP_BIN="$TMP_BIN"

# 1) 系统用户（无登录 shell，只跑服务）
if ! id -u rekosync >/dev/null 2>&1; then
  useradd -r -s /usr/sbin/nologin rekosync
  echo "  已创建系统用户 rekosync"
fi

# 2) 目录与权限
mkdir -p "\$REMOTE_DIR/data"
install -m 0755 "\$TMP_BIN" "\$REMOTE_DIR/rekosync"
rm -f "\$TMP_BIN"
chown -R rekosync:rekosync "\$REMOTE_DIR"
chmod 700 "\$REMOTE_DIR/data"
echo "  已安装 \$REMOTE_DIR/rekosync"

# 3) 建同步账号（账号已存在则跳过，不覆盖口令）
if [ -n "\$ACCOUNT" ]; then
  if printf '%s\n' "\$PASSWORD" | "\$REMOTE_DIR/rekosync" -data "\$REMOTE_DIR/data" -adduser "\$ACCOUNT" 2>/dev/null; then
    echo "  已创建账号 \$ACCOUNT"
  else
    echo "  [!] 账号 \$ACCOUNT 可能已存在，跳过（需要改口令请手动执行 -passwd）"
  fi
  chown -R rekosync:rekosync "\$REMOTE_DIR/data"
fi

# 4) systemd
cat > /etc/systemd/system/rekosync.service <<UNIT
[Unit]
Description=RekoBox node sync server (rekosync)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=rekosync
Group=rekosync
WorkingDirectory=\$REMOTE_DIR
ExecStart=\$REMOTE_DIR/rekosync -addr \$LISTEN -data \$REMOTE_DIR/data
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=\$REMOTE_DIR/data
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable rekosync >/dev/null 2>&1 || true
systemctl restart rekosync
sleep 1

if systemctl is-active --quiet rekosync; then
  echo "  服务已启动"
else
  echo "  [!] 服务未起来，最近日志："
  journalctl -u rekosync -n 20 --no-pager || true
  exit 1
fi

# 5) 防火墙（只在明确要求时动）
if [ "\$OPEN_FW" = "y" ]; then
  FW_PORT="\${LISTEN##*:}"
  if command -v ufw >/dev/null 2>&1; then
    ufw allow "\$FW_PORT"/tcp >/dev/null 2>&1 && echo "  ufw 已放行 \$FW_PORT/tcp" || echo "  [!] ufw 放行失败"
  elif command -v firewall-cmd >/dev/null 2>&1; then
    firewall-cmd --permanent --add-port="\$FW_PORT"/tcp >/dev/null 2>&1 && firewall-cmd --reload >/dev/null 2>&1 \
      && echo "  firewalld 已放行 \$FW_PORT/tcp" || echo "  [!] firewalld 放行失败"
  else
    echo "  [!] 未检测到 ufw/firewalld，请自行在安全组放行 \$FW_PORT/tcp"
  fi
fi

# 6) 本机自检
HEALTH_PORT="\${LISTEN##*:}"
CHECK_HOST=127.0.0.1
case "\$LISTEN" in
  0.0.0.0:*|:*) CHECK_HOST=127.0.0.1 ;;
  \[*\]:*) CHECK_HOST=127.0.0.1 ;;
  *) CHECK_HOST="\${LISTEN%%:*}" ;;
esac
if command -v curl >/dev/null 2>&1; then
  echo -n "  健康检查: "
  curl -fsS "http://\$CHECK_HOST:\$HEALTH_PORT/api/v1/health" || echo "(curl 无响应，看 journalctl -u rekosync)"
  echo
fi
REMOTE

# ---------- 7. 收尾 ----------

echo
say "部署完成"
PUB_HOST="$HOST"
FW_PORT="${LISTEN##*:}"
cat <<EOF

  服务端      systemctl status rekosync --no-pager
  日志        journalctl -u rekosync -f
  数据目录    $REMOTE_DIR/data（账号、令牌、节点快照；备份就 tar 这个目录）

  客户端填    http://$PUB_HOST:$FW_PORT

EOF

if [ -n "$ACCOUNT" ]; then
  if [ "$GEN_PASS" = "y" ]; then
    echo "  账号        $ACCOUNT"
    printf '  口令        %s%s%s   %s(仅显示这一次，请立刻保存)%s\n' "$C_OK" "$PASSWORD" "$C_END" "$C_WARN" "$C_END"
    note "若上面提示「账号已存在」，说明口令没被改动，请用下面的改口令命令重设"
  else
    echo "  账号        $ACCOUNT（口令为你刚才输入的那一个）"
  fi
  echo
  note "改口令：ssh $USER@$HOST '$PRIV $REMOTE_DIR/rekosync -data $REMOTE_DIR/data -passwd $ACCOUNT'"
  note "删账号：ssh $USER@$HOST '$PRIV $REMOTE_DIR/rekosync -data $REMOTE_DIR/data -deluser $ACCOUNT'"
fi

warn "当前是 HTTP 明文：口令与节点配置在链路上可见。"
note "公网长期使用请套 TLS：把 LISTEN 改成 127.0.0.1:8080，前面挂 nginx，客户端地址改 https://"
echo
