#!/usr/bin/env bash
# ==============================================================================
# rekosync 一键安装（在服务器上运行，全新系统也能跑）
#
#   bash deploy.sh                 # 交互式：装依赖 → 拉源码 → 编译 → 装服务 → 建账号
#   bash deploy.sh -y              # 全部默认值，不询问
#   bash deploy.sh -h              # 看全部参数
#
#   在仓库里运行（脚本旁有 main.go）  → 直接用本地源码
#   在别处运行（比如 /root 或 /tmp）→ 自动 git clone，失败再退回源码包下载
#
# 支持：Ubuntu/Debian、CentOS/RHEL/Rocky/Alma、Alpine；x86_64 / arm64
# 需要：root（非 root 会自动 sudo）、能访问外网（可用 GOPROXY / REPO_URL 换源）
# ==============================================================================
set -Eeuo pipefail

# 记住原始参数：非 root 需要 sudo 重新执行时不能丢参数
ORIG_ARGS=("$@")

# ---------- 输出 ----------
if [ -t 1 ]; then
  C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
  C_OK=''; C_WARN=''; C_ERR=''; C_DIM=''; C_END=''
fi
say()  { printf '%s==>%s %s\n' "$C_OK" "$C_END" "$*"; }
note() { printf '%s    %s%s\n' "$C_DIM" "$*" "$C_END"; }
warn() { printf '%s[!]%s %s\n' "$C_WARN" "$C_END" "$*" >&2; }
die()  { printf '%s[x]%s %s\n' "$C_ERR" "$C_END" "$*" >&2; exit 1; }
step() { printf '\n%s==>%s %s\n' "$C_OK" "$C_END" "$*"; }

trap 'rc=$?; if [ $rc -ne 0 ]; then printf "\n%s[x]%s 第 %s 行失败（退出码 %s）。日志：journalctl -u rekosync -n 50\n" "$C_ERR" "$C_END" "$LINENO" "$rc" >&2; fi' ERR

# ---------- 参数 ----------
LISTEN=":8080"
INSTALL_DIR="/opt/rekosync"
ACCOUNT="alice"
PASSWORD=""
PASSWORD_SET=0
CREATE_ACCOUNT=1
ASSUME_YES=0
SKIP_DEPS=0
WITH_FIREWALL=""
BRANCH="main"
REPO_URL="${REPO_URL:-https://github.com/tmclsamxy/RekoBox.git}"
SRC_DIR=""
BINARY=""
RESET_PASSWORD=0

usage() {
  cat <<'EOF'
用法: bash deploy.sh [选项]

  -h, --help           显示帮助
  -y, --yes            全部用默认值，不交互（适合脚本化）
      --listen ADDR    监听地址，默认 :8080（只给 nginx 反代就写 127.0.0.1:8080）
      --dir PATH       安装目录，默认 /opt/rekosync
      --account NAME   同步账号名，默认 alice
      --password PASS  账号口令；不填则交互输入，留空自动生成随机口令
                     传 - 表示从标准输入读一行（远程部署时用，口令不进命令行）
      --no-account     跳过建号（只装服务）
      --reset-password 账号已存在时用 --password 覆盖口令
      --branch REF     拉源码用的分支/标签，默认 main
      --repo URL       源码仓库地址（内网镜像或 fork 时用）
      --src DIR        直接用本地源码目录，不 clone
      --binary PATH    直接安装现成的 Linux 二进制（跳过依赖、源码与编译）
      --skip-deps      不装系统依赖（已装好 go 时用）
      --firewall       自动放行防火墙端口（默认询问）
      --no-firewall    不动防火墙

环境变量: REPO_URL / GOPROXY / GO_VERSION
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -y|--yes) ASSUME_YES=1 ;;
    --listen) LISTEN="${2:?缺参数}"; shift ;;
    --dir) INSTALL_DIR="${2:?缺参数}"; shift ;;
    --account) ACCOUNT="${2:?缺参数}"; shift ;;
    --password) PASSWORD="${2:?缺参数}"; PASSWORD_SET=1; shift ;;
    --no-account) CREATE_ACCOUNT=0 ;;
    --reset-password) RESET_PASSWORD=1 ;;
    --branch) BRANCH="${2:?缺参数}"; shift ;;
    --repo) REPO_URL="${2:?缺参数}"; shift ;;
    --src) SRC_DIR="${2:?缺参数}"; shift ;;
    --binary) BINARY="${2:?缺参数}"; shift ;;
    --skip-deps) SKIP_DEPS=1 ;;
    --firewall) WITH_FIREWALL=1 ;;
    --no-firewall) WITH_FIREWALL=0 ;;
    *) die "未知参数：$1（-h 看帮助）" ;;
  esac
  shift
done

# --password - ：从标准输入读一行，供远程部署管道使用（口令不出现在 ps 里）
if [ "$PASSWORD" = "-" ]; then
  IFS= read -r PASSWORD || true
  PASSWORD_SET=1
fi

# ---------- 交互工具 ----------
have_tty() { [ -t 0 ]; }

ask() { # ask <提示> <默认> <变量名>
  local prompt="$1" def="$2" var="$3" input=""
  if ! have_tty || [ "$ASSUME_YES" = 1 ]; then return; fi
  read -r -p "$prompt [$def]: " input || true
  printf -v "$var" '%s' "${input:-$def}"
}

ask_yn() { # ask_yn <提示> <默认> <变量名>
  local prompt="$1" def="$2" var="$3" input=""
  if [ -n "${!var:-}" ]; then return; fi
  if ! have_tty || [ "$ASSUME_YES" = 1 ]; then printf -v "$var" '%s' "$def"; return; fi
  read -r -p "$prompt [$def]: " input || true
  case "${input:-$def}" in y|Y|yes|YES) printf -v "$var" '%s' "y" ;; *) printf -v "$var" '%s' "n" ;; esac
}

rand_pass() {
  local p
  p=$(LC_ALL=C tr -dc 'A-Za-z2-9' </dev/urandom 2>/dev/null | head -c 20) || true
  if [ -n "$p" ]; then printf '%s' "$p"; else printf 'Reko-%s' "$(date +%s)"; fi
}

# ---------- 0. 前置 ----------
[ "$(uname -s)" = "Linux" ] || die "本脚本要在 Linux 服务器上运行；从 Windows/macOS 远程部署请用 deploy-remote.sh"

if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    say "非 root，自动用 sudo 重新执行"
    exec sudo -E bash "$0" "${ORIG_ARGS[@]}"
  fi
  die "需要 root 权限"
fi

OS_NAME="Linux"
if [ -f /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  OS_NAME="${PRETTY_NAME:-${ID:-Linux}}"
fi

ARCH_RAW="$(uname -m)"
case "$ARCH_RAW" in
  x86_64|amd64) GO_ARCH="amd64" ;;
  aarch64|arm64) GO_ARCH="arm64" ;;
  *) die "不支持的 CPU 架构：$ARCH_RAW（Go 也只剩 amd64/arm64 可用）" ;;
esac

echo
say "RekoBox 节点同步服务 · 一键安装"
note "系统：$OS_NAME ($ARCH_RAW)"
note "安装目录：$INSTALL_DIR    监听：$LISTEN"

# ---------- 1. 系统依赖 ----------
PM=""
if command -v apt-get >/dev/null 2>&1; then PM=apt
elif command -v dnf >/dev/null 2>&1; then PM=dnf
elif command -v yum >/dev/null 2>&1; then PM=yum
elif command -v apk >/dev/null 2>&1; then PM=apk
fi

pkg_install() {
  case "$PM" in
    apt) DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$@" >/dev/null ;;
    dnf) dnf install -y -q "$@" >/dev/null ;;
    yum) yum install -y -q "$@" >/dev/null ;;
    apk) apk add --no-cache "$@" >/dev/null ;;
    *) return 1 ;;
  esac
}

step "1/6 系统依赖"
if [ -n "$BINARY" ]; then
  note "安装现成二进制，跳过"
elif [ "$SKIP_DEPS" = 1 ]; then
  note "按要求跳过"
else
  if [ -z "$PM" ]; then
    warn "没识别到包管理器，跳过（需要 curl / tar / git 已就绪）"
  else
    note "包管理器：$PM"
    if [ "$PM" = "apt" ]; then apt-get update -qq >/dev/null 2>&1 || warn "apt-get update 失败，继续"; fi
    MISSING=""
    for c in curl tar git; do
      command -v "$c" >/dev/null 2>&1 || MISSING="$MISSING $c"
    done
    if [ -n "$MISSING" ]; then
      note "安装：$MISSING ca-certificates"
      pkg_install $MISSING ca-certificates || die "依赖安装失败，请手动安装：$MISSING"
    else
      note "curl / tar / git 已就绪"
    fi
  fi
fi
command -v curl >/dev/null 2>&1 || die "缺少 curl，无法继续"

# ---------- 2. Go ----------
GO_MIN_MAJOR=1
GO_MIN_MINOR=21
GO_VERSION="${GO_VERSION:-1.22.12}"

go_ok() {
  command -v go >/dev/null 2>&1 || return 1
  local v maj min
  v="$(go env GOVERSION 2>/dev/null | sed -E 's/^go//')"
  [ -n "$v" ] || return 1
  maj="$(printf '%s' "$v" | cut -d. -f1)"
  min="$(printf '%s' "$v" | cut -d. -f2)"
  [ "$maj" -gt "$GO_MIN_MAJOR" ] || { [ "$maj" -eq "$GO_MIN_MAJOR" ] && [ "$min" -ge "$GO_MIN_MINOR" ]; }
}

install_go() {
  local tarball="go${GO_VERSION}.linux-${GO_ARCH}.tar.gz"
  local tmp="/tmp/${tarball}"
  local url ok=0
  for url in \
    "https://mirrors.aliyun.com/golang/${tarball}" \
    "https://golang.google.cn/dl/${tarball}" \
    "https://go.dev/dl/${tarball}"; do
    note "下载 $url"
    if curl -fL --connect-timeout 10 --retry 2 -o "$tmp" "$url" 2>/dev/null; then ok=1; break; fi
    warn "下载失败，换下一个源"
  done
  [ "$ok" = 1 ] || die "Go 下载失败。可手动安装 Go >= ${GO_MIN_MAJOR}.${GO_MIN_MINOR} 后重试，或加 --skip-deps"
  note "解压到 /usr/local/go"
  rm -rf /usr/local/go
  tar -C /usr/local -xzf "$tmp" || die "解压失败"
  rm -f "$tmp"
  ln -sf /usr/local/go/bin/go /usr/local/bin/go
  ln -sf /usr/local/go/bin/gofmt /usr/local/bin/gofmt
  export PATH="/usr/local/go/bin:$PATH"
}

step "2/6 Go 工具链"
if [ -n "$BINARY" ]; then
  note "安装现成二进制，不需要 Go"
elif go_ok; then
  note "已安装 $(go env GOVERSION)"
elif [ "$SKIP_DEPS" = 1 ]; then
  die "--skip-deps 指定跳过，但当前没有 Go >= ${GO_MIN_MAJOR}.${GO_MIN_MINOR}"
else
  note "未检测到 Go >= ${GO_MIN_MAJOR}.${GO_MIN_MINOR}，安装 go${GO_VERSION}.linux-${GO_ARCH}"
  install_go
  go_ok || die "Go 安装后仍不可用：$(command -v go || echo 未找到)"
fi

# ---------- 3. 源码 ----------
step "3/6 获取源码"
if [ -n "$BINARY" ]; then
  note "安装现成二进制，跳过"
else
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "$SRC_DIR" ] && [ -f "$SCRIPT_DIR/main.go" ] && [ -f "$SCRIPT_DIR/go.mod" ]; then
  SRC_DIR="$SCRIPT_DIR"
  note "使用当前目录源码：$SRC_DIR"
fi

if [ -z "$SRC_DIR" ]; then
  CLONE_DIR="/opt/rekobox-src"
  if [ -d "$CLONE_DIR/.git" ]; then
    note "已有源码：$CLONE_DIR，尝试更新到 $BRANCH"
    if git -C "$CLONE_DIR" fetch --depth 1 origin "$BRANCH" >/dev/null 2>&1 \
      && git -C "$CLONE_DIR" checkout -q FETCH_HEAD; then
      note "已更新"
    else
      warn "更新失败（网络或分支问题），沿用现有源码"
    fi
  else
    note "克隆 $REPO_URL（分支 $BRANCH）"
    rm -rf "$CLONE_DIR"
    if git clone --depth 1 --branch "$BRANCH" "$REPO_URL" "$CLONE_DIR" 2>&1 | tail -3; then
      note "克隆完成"
    else
      warn "git clone 失败，改下载源码包"
      TARBALL="/tmp/rekobox-src.tar.gz"
      OK=0
      for url in \
        "https://codeload.github.com/tmclsamxy/RekoBox/tar.gz/refs/heads/${BRANCH}" \
        "https://github.com/tmclsamxy/RekoBox/archive/refs/heads/${BRANCH}.tar.gz"; do
        note "下载 $url"
        if curl -fL --connect-timeout 10 --retry 2 -o "$TARBALL" "$url" 2>/dev/null; then OK=1; break; fi
      done
      [ "$OK" = 1 ] || die "源码获取失败。可加 --src 指向已上传的源码目录，或 --repo 换可达镜像"
      rm -rf "$CLONE_DIR"
      mkdir -p "$CLONE_DIR"
      tar -xzf "$TARBALL" -C "$CLONE_DIR" --strip-components=1 || die "源码包解压失败"
      rm -f "$TARBALL"
    fi
  fi
  SRC_DIR="$CLONE_DIR/RekoboxForWindows/server"
fi

[ -f "$SRC_DIR/main.go" ] || die "源码不完整：$SRC_DIR 下没有 main.go"
note "源码目录：$SRC_DIR"
fi

# ---------- 4. 编译 ----------
step "4/6 编译"
if [ -n "$BINARY" ]; then
  BIN_TMP="$BINARY"
  [ -s "$BIN_TMP" ] || die "找不到可用的二进制：$BIN_TMP"
  note "使用现成二进制：$BIN_TMP（$(du -h "$BIN_TMP" | cut -f1)）"
else
  export CGO_ENABLED=0
  export GOFLAGS="-mod=mod"
  export GOPROXY="${GOPROXY:-https://goproxy.cn,direct}"
  export GOTOOLCHAIN=local
  note "GOPROXY=$GOPROXY"
  BIN_TMP="/tmp/rekosync.build.$$"
  if ! ( cd "$SRC_DIR" && go build -trimpath -ldflags "-s -w" -o "$BIN_TMP" . ); then
    rm -f "$BIN_TMP"
    die "编译失败。若在墙内：GOPROXY=https://goproxy.cn,direct bash deploy.sh"
  fi
  [ -s "$BIN_TMP" ] || die "编译产物为空"
  note "产物大小：$(du -h "$BIN_TMP" | cut -f1)"
fi

# ---------- 5. 安装 ----------
step "5/6 安装服务"

if [ "$CREATE_ACCOUNT" = 1 ] && [ "$PASSWORD_SET" = 0 ] && have_tty && [ "$ASSUME_YES" = 0 ]; then
  ask "同步账号名（小写字母/数字，2-32 位）" "$ACCOUNT" ACCOUNT
  read -r -s -p "账号口令（至少 6 位，直接回车自动生成）: " PASSWORD || true
  echo
  if [ -n "$PASSWORD" ]; then PASSWORD_SET=1; fi
fi
GEN_PASS=0
if [ "$CREATE_ACCOUNT" = 1 ] && [ -z "$PASSWORD" ]; then
  PASSWORD="$(rand_pass)"; GEN_PASS=1
fi
if [ "$CREATE_ACCOUNT" = 1 ] && [ "${#PASSWORD}" -lt 6 ]; then
  die "口令至少 6 位"
fi

# 显式建组：useradd -r 在部分发行版不会自动创建同名私有组，
# 而 systemd 单元的 Group=rekosync 找不到组会直接启动失败
if ! getent group rekosync >/dev/null 2>&1; then
  if command -v groupadd >/dev/null 2>&1; then
    groupadd -r rekosync || warn "创建组 rekosync 失败"
  else
    addgroup -S rekosync 2>/dev/null || warn "创建组 rekosync 失败"
  fi
fi

if ! id -u rekosync >/dev/null 2>&1; then
  if command -v useradd >/dev/null 2>&1; then
    useradd -r -g rekosync -s /usr/sbin/nologin rekosync
  else
    adduser -S -D -H -G rekosync -s /sbin/nologin rekosync
  fi
  note "已创建系统用户 rekosync（组 rekosync）"
fi

mkdir -p "$INSTALL_DIR/data"
install -m 0755 "$BIN_TMP" "$INSTALL_DIR/rekosync"
# 只清理自己下下来的临时文件，不动用户用 --binary 指定的路径
case "$BIN_TMP" in
  /tmp/rekosync.*) rm -f "$BIN_TMP" ;;
esac
note "二进制：$INSTALL_DIR/rekosync"

if [ "$CREATE_ACCOUNT" = 1 ]; then
  if [ "$RESET_PASSWORD" = 1 ]; then
    if printf '%s\n' "$PASSWORD" | "$INSTALL_DIR/rekosync" -data "$INSTALL_DIR/data" -passwd "$ACCOUNT" >/dev/null 2>&1; then
      note "已重置账号 $ACCOUNT 的口令（旧令牌已吊销）"
    else
      warn "重置口令失败（账号可能不存在，下面会尝试创建）"
    fi
  fi
  ADD_RC=0
  ADD_OUT="$(printf '%s\n' "$PASSWORD" | "$INSTALL_DIR/rekosync" -data "$INSTALL_DIR/data" -adduser "$ACCOUNT" 2>&1)" || ADD_RC=$?
  if [ "$ADD_RC" = 0 ]; then
    note "已创建账号 $ACCOUNT"
  elif printf '%s' "$ADD_OUT" | grep -qiE "exist|已存在"; then
    note "账号 $ACCOUNT 已存在，跳过（口令未改动；要改口令加 --reset-password）"
  else
    warn "建号失败：$(printf '%s' "$ADD_OUT" | tail -1)"
  fi
fi

chown -R rekosync:rekosync "$INSTALL_DIR" 2>/dev/null || true
chmod 700 "$INSTALL_DIR/data"

mkdir -p /etc/systemd/system
cat > /etc/systemd/system/rekosync.service <<UNIT
[Unit]
Description=RekoBox node sync server (rekosync)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=rekosync
Group=rekosync
WorkingDirectory=${INSTALL_DIR}
ExecStart=${INSTALL_DIR}/rekosync -addr ${LISTEN} -data ${INSTALL_DIR}/data
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=${INSTALL_DIR}/data
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable rekosync >/dev/null 2>&1 || true
systemctl restart rekosync
sleep 1
if ! systemctl is-active --quiet rekosync; then
  journalctl -u rekosync -n 30 --no-pager || true
  die "服务启动失败（见上面日志）"
fi
note "systemd 状态：active"

# ---------- 6. 防火墙与自检 ----------
step "6/6 防火墙与自检"

FW_PORT="${LISTEN##*:}"
case "$LISTEN" in
  *:*:*) FW_PORT="${LISTEN##*:}" ;;
esac

if [ -z "$WITH_FIREWALL" ]; then
  ask_yn "放行防火墙端口 ${FW_PORT}/tcp" "y" WITH_FIREWALL
fi
if [ "$WITH_FIREWALL" = "y" ]; then
  if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -qi "Status: active"; then
    ufw allow "${FW_PORT}/tcp" >/dev/null 2>&1 && note "ufw 已放行 ${FW_PORT}/tcp" || warn "ufw 放行失败"
  elif command -v firewall-cmd >/dev/null 2>&1 && systemctl is-active --quiet firewalld; then
    firewall-cmd --permanent --add-port="${FW_PORT}/tcp" >/dev/null 2>&1 \
      && firewall-cmd --reload >/dev/null 2>&1 \
      && note "firewalld 已放行 ${FW_PORT}/tcp" || warn "firewalld 放行失败"
  else
    note "未检测到启用的 ufw/firewalld，跳过"
  fi
fi

CHECK_HOST="127.0.0.1"
case "$LISTEN" in
  :*|0.0.0.0:*) CHECK_HOST="127.0.0.1" ;;
  *) CHECK_HOST="${LISTEN%%:*}" ;;
esac

HEALTH_OK=0
HEALTH_BODY=""
for _ in 1 2 3 4 5; do
  if HEALTH_BODY="$(curl -fsS --max-time 3 "http://${CHECK_HOST}:${FW_PORT}/api/v1/health" 2>/dev/null)"; then
    HEALTH_OK=1
    break
  fi
  sleep 1
done

echo
if [ "$HEALTH_OK" = 1 ]; then
  say "健康检查通过：$HEALTH_BODY"
else
  warn "健康检查未通过，看日志：journalctl -u rekosync -n 50 --no-pager"
fi

PUB_IP="$(curl -fsS --max-time 3 https://api.ipify.org 2>/dev/null || curl -fsS --max-time 3 https://ipv4.icanhazip.com 2>/dev/null || true)"
[ -n "$PUB_IP" ] || PUB_IP="<服务器公网IP>"

cat <<EOF

──────────────── 安装完成 ────────────────
  服务      systemctl status rekosync --no-pager
  日志      journalctl -u rekosync -f
  数据      $INSTALL_DIR/data（账号/令牌/节点快照，备份就 tar 这个目录）
  客户端填  http://$PUB_IP:$FW_PORT
EOF

if [ "$CREATE_ACCOUNT" = 1 ]; then
  echo "  账号      $ACCOUNT"
  if [ "$GEN_PASS" = 1 ]; then
    printf '  口令      %s%s%s  %s(只显示这一次，请立刻保存)%s\n' "$C_OK" "$PASSWORD" "$C_END" "$C_WARN" "$C_END"
    note "若上面提示「账号已存在」，这个口令并未生效，请用 --reset-password 重设"
  else
    echo "  口令      你刚才输入的那一个"
  fi
  echo
  note "改口令：$INSTALL_DIR/rekosync -data $INSTALL_DIR/data -passwd $ACCOUNT"
  note "删账号：$INSTALL_DIR/rekosync -data $INSTALL_DIR/data -deluser $ACCOUNT"
  note "列账号：$INSTALL_DIR/rekosync -data $INSTALL_DIR/data -users"
fi

echo
warn "HTTP 明文：口令与节点配置在链路上可见。"
note "公网长期使用：--listen 127.0.0.1:8080 前置 nginx + TLS，客户端地址改 https://"
echo
