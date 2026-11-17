#!/usr/bin/env bash
# ==============================================================================
# RekoBox 服务端（rekosync）部署工具  v2.1.0
#
#   一键执行（服务器上，任意目录）：
#     curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh | sudo bash
#     # 若交互提示没出现（某些终端的管道限制），改用：
#     sudo bash <(curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh)
#
#   交互菜单：1 安装 / 2 更新 / 3 清理卸载 / 0 退出
#   也可直接指定动作（脚本化 / 远程部署）：
#     bash deploy.sh --install -y --port 9000 --account root
#     bash deploy.sh --update
#     bash deploy.sh --uninstall -y
#
#   在仓库目录里运行（脚本旁有 main.go）→ 直接用本地源码；
#   在别处运行 → 自动 git clone（失败再下载源码包）到 /opt/rekobox-src。
#
# 支持：Ubuntu/Debian、CentOS/RHEL/Rocky/Alma、Alpine；x86_64 / arm64
# 需要：root（非 root 会自动 sudo）、能访问外网（可用 GOPROXY / REPO_URL 换源）
# ==============================================================================
set -Eeuo pipefail

TOOL_VERSION="2.1.0"
SELF_URL="${SELF_URL:-https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh}"
SELF="${BASH_SOURCE[0]:-}"
ORIG_ARGS=("$@")

# ---------- 输出 ----------
if [ -t 1 ]; then
  C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_DIM=$'\033[2m'
  C_B=$'\033[1m'; C_END=$'\033[0m'
else
  C_OK=''; C_WARN=''; C_ERR=''; C_DIM=''; C_B=''; C_END=''
fi
say()  { printf '%s==>%s %s\n' "$C_OK" "$C_END" "$*"; }
note() { printf '%s    %s%s\n' "$C_DIM" "$*" "$C_END"; }
warn() { printf '%s[!]%s %s\n' "$C_WARN" "$C_END" "$*" >&2; }
die()  { printf '%s[x]%s %s\n' "$C_ERR" "$C_END" "$*" >&2; exit 1; }
step() { printf '\n%s==>%s %s\n' "$C_OK" "$C_END" "$*"; }

trap 'rc=$?; if [ $rc -ne 0 ]; then printf "\n%s[x]%s 第 %s 行失败（退出码 %s）。排查：journalctl -u rekosync -n 50\n" "$C_ERR" "$C_END" "$LINENO" "$rc" >&2; fi' ERR

# ---------- 终端 ----------
# 支持 `curl … | bash`：此时 stdin 是脚本管道，交互必须走 /dev/tty
TTY=""
if [ -r /dev/tty ] && [ -w /dev/tty ]; then TTY="/dev/tty"; fi
have_tty() { [ -n "$TTY" ]; }

# ---------- 默认参数 ----------
ACTION=""            # install | update | uninstall
SERVICE_PORT="${SERVICE_PORT:-8080}"
PORT_SET=0
LISTEN=""
LISTEN_SET=0
INSTALL_DIR="/opt/rekosync"
DIR_SET=0
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
KEEP_DATA=0
FORCE=0
GEN_PASS=0
BUILT_TMP=0

usage() {
  cat <<'EOF'
用法: bash deploy.sh [动作] [选项]
      不带动作时进入交互菜单（1 安装 / 2 更新 / 3 清理卸载）

动作
      --install            安装（已安装时默认拒绝，加 --force 覆盖程序、保留数据）
      --update             更新到最新版本（保留账号与节点池）
      --uninstall          完全卸载（服务/程序/数据/系统用户/防火墙规则）
      -h, --help           显示帮助

安装参数
      --port N             服务端口，默认 8080
      --listen ADDR        完整监听地址，如 127.0.0.1:8080（前置 nginx 时用）
      --dir PATH           安装目录，默认 /opt/rekosync
      --account NAME       管理员账号名，默认 alice（登录网页控制台用）
      --password PASS      管理员口令；留空则交互输入，直接回车自动生成随机口令
                           传 - 表示从标准输入读一行（远程部署用，口令不进 ps）
      --no-account         跳过建号（只装服务）
      --reset-password     账号已存在时用 --password 覆盖口令
      --firewall           自动放行防火墙端口
      --no-firewall        不动防火墙

通用
      -y, --yes            不交互，全部用默认值 / 直接执行（适合脚本化）
      --force              安装时允许覆盖已存在的安装
      --keep-data          卸载时先把数据打包备份到 /var/backups
      --branch REF         拉源码用的分支/标签，默认 main
      --repo URL           源码仓库地址（内网镜像或 fork 时用）
      --src DIR            直接用本地源码目录，不 clone
      --binary PATH        直接安装现成的 Linux 二进制（跳过依赖、源码与编译）
      --skip-deps          不装系统依赖（已装好 go 时用）

环境变量: REPO_URL / SELF_URL / GOPROXY / GO_VERSION
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --install) ACTION=install ;;
    --update) ACTION=update ;;
    --uninstall|--remove) ACTION=uninstall ;;
    -y|--yes) ASSUME_YES=1 ;;
    --force) FORCE=1 ;;
    --keep-data) KEEP_DATA=1 ;;
    --listen) LISTEN="${2:?缺参数}"; LISTEN_SET=1; shift ;;
    --port) SERVICE_PORT="${2:?缺参数}"; PORT_SET=1; shift ;;
    --dir) INSTALL_DIR="${2:?缺参数}"; DIR_SET=1; shift ;;
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

# --password - ：从标准输入读一行（远程部署管道用）
if [ "$PASSWORD" = "-" ]; then
  IFS= read -r PASSWORD || true
  PASSWORD_SET=1
fi

# ---------- 交互工具 ----------
ask() { # ask <提示> <默认> <变量名>
  local prompt="$1" def="$2" var="$3" input=""
  if ! have_tty || [ "$ASSUME_YES" = 1 ]; then printf -v "$var" '%s' "$def"; return; fi
  printf '%s' "$prompt [$def]: " >&2
  IFS= read -r input < "$TTY" || true
  printf -v "$var" '%s' "${input:-$def}"
}

ask_yn() { # ask_yn <提示> <默认 y/n> <变量名>
  local prompt="$1" def="$2" var="$3" input=""
  if [ -n "${!var:-}" ]; then return; fi
  if ! have_tty || [ "$ASSUME_YES" = 1 ]; then printf -v "$var" '%s' "$def"; return; fi
  printf '%s' "$prompt [$def]: " >&2
  IFS= read -r input < "$TTY" || true
  case "${input:-$def}" in y|Y|yes|YES) printf -v "$var" '%s' "y" ;; *) printf -v "$var" '%s' "n" ;; esac
}

confirm_word() { # confirm_word <提示> <要输入的词>
  local prompt="$1" word="$2" input=""
  if ! have_tty; then die "$prompt：无终端无法确认（脚本化请加 -y）"; fi
  printf '%s（输入 %s 确认）: ' "$prompt" "$word" >&2
  IFS= read -r input < "$TTY" || true
  [ "$input" = "$word" ] || die "已取消"
}

rand_pass() {
  local p
  p=$(LC_ALL=C tr -dc 'A-Za-z2-9' </dev/urandom 2>/dev/null | head -c 20) || true
  if [ -n "$p" ]; then printf '%s' "$p"; else printf 'Reko-%s' "$(date +%s)"; fi
}

valid_port() {
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  [ "$1" -ge 1 ] && [ "$1" -le 65535 ]
}

# ---------- 0. 前置 ----------
[ "$(uname -s)" = "Linux" ] || die "本脚本要在 Linux 服务器上运行；从 Windows/macOS 远程部署请用 deploy-remote.sh"

if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    say "非 root，自动用 sudo 重新执行"
    if [ -n "$SELF" ] && [ -f "$SELF" ]; then
      exec sudo -E bash "$SELF" ${ORIG_ARGS[@]+"${ORIG_ARGS[@]}"}
    fi
    # 管道运行（curl | bash）时没有脚本文件，重新下载一份再提权
    exec sudo -E bash -c "$(curl -fsSL "$SELF_URL")" rekosync-deploy ${ORIG_ARGS[@]+"${ORIG_ARGS[@]}"}
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

UNIT_PATH="/etc/systemd/system/rekosync.service"
SRC_CACHE="/opt/rekobox-src"
# 记住上次安装到哪，更新/卸载时不用重复 --dir
POINTER_FILE="/etc/rekobox-install.conf"

# ---------- 安装状态 ----------
has_unit() { [ -f "$UNIT_PATH" ]; }
# 三者任一存在都算「装过」：二进制、目录、单元文件（单元残留也要能被卸载掉）
is_installed() { [ -f "$INSTALL_DIR/rekosync" ] || [ -d "$INSTALL_DIR" ] || has_unit; }

load_conf() {
  # 自定义安装目录时，靠指针文件找到真正的 install.conf
  if [ "$DIR_SET" = 0 ] && [ ! -f "$INSTALL_DIR/install.conf" ] && [ -f "$POINTER_FILE" ]; then
    # shellcheck disable=SC1090
    . "$POINTER_FILE"
    INSTALL_DIR="${CONF_DIR:-$INSTALL_DIR}"
  fi
  local conf="$INSTALL_DIR/install.conf"
  [ -f "$conf" ] || return 0
  # shellcheck disable=SC1090
  . "$conf"
  INSTALL_DIR="${CONF_DIR:-$INSTALL_DIR}"
  SERVICE_PORT="${CONF_PORT:-$SERVICE_PORT}"
  ACCOUNT="${CONF_ACCOUNT:-$ACCOUNT}"
  if [ -n "${CONF_LISTEN:-}" ]; then
    LISTEN="$CONF_LISTEN"
    LISTEN_SET=1
    SERVICE_PORT="${LISTEN##*:}"
  fi
}

installed_version() {
  [ -f "$INSTALL_DIR/rekosync" ] || return 0
  "$INSTALL_DIR/rekosync" -version 2>/dev/null | head -1 || true
}

service_state() {
  if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet rekosync 2>/dev/null; then
    printf '运行中'
  elif [ -f "$UNIT_PATH" ]; then
    printf '已停止'
  else
    printf '未安装'
  fi
}

detect_port_from_unit() {
  [ -f "$UNIT_PATH" ] || return 0
  local a
  a="$(sed -n 's/^ExecStart=.*-addr[ =]\([^ ]*\).*/\1/p' "$UNIT_PATH" | head -1)"
  [ -n "$a" ] || return 0
  SERVICE_PORT="${a##*:}"
}

# ---------- 包管理 ----------
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

ensure_basics() {
  if [ -z "$PM" ]; then
    warn "没识别到包管理器，跳过依赖安装（需要 curl / tar 已就绪）"
    return 0
  fi
  note "包管理器：$PM"
  if [ "$PM" = "apt" ]; then apt-get update -qq >/dev/null 2>&1 || warn "apt-get update 失败，继续"; fi
  local MISSING="" c
  for c in curl tar; do command -v "$c" >/dev/null 2>&1 || MISSING="$MISSING $c"; done
  if [ -n "$MISSING" ]; then
    note "安装：$MISSING ca-certificates"
    pkg_install $MISSING ca-certificates || die "依赖安装失败，请手动安装：$MISSING"
  else
    note "curl / tar 已就绪"
  fi
}

ensure_git() {
  command -v git >/dev/null 2>&1 && return 0
  [ -n "$PM" ] || return 1
  note "安装 git"
  pkg_install git || return 1
  command -v git >/dev/null 2>&1
}

# ---------- Go ----------
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

# ---------- 源码 ----------
download_source_tar() {
  local TARBALL="/tmp/rekobox-src.tar.gz" OK=0 url
  for url in \
    "https://codeload.github.com/tmclsamxy/RekoBox/tar.gz/refs/heads/${BRANCH}" \
    "https://github.com/tmclsamxy/RekoBox/archive/refs/heads/${BRANCH}.tar.gz"; do
    note "下载 $url"
    if curl -fL --connect-timeout 10 --retry 2 -o "$TARBALL" "$url" 2>/dev/null; then OK=1; break; fi
  done
  [ "$OK" = 1 ] || die "源码获取失败。可加 --src 指向已上传的源码目录，或 --repo 换可达镜像"
  rm -rf "$SRC_CACHE"
  mkdir -p "$SRC_CACHE"
  tar -xzf "$TARBALL" -C "$SRC_CACHE" --strip-components=1 || die "源码包解压失败"
  rm -f "$TARBALL"
}

fetch_source() {
  step "获取源码"
  if [ -n "$BINARY" ]; then
    note "使用现成二进制，跳过"
    return 0
  fi

  if [ -z "$SRC_DIR" ] && [ -n "$SELF" ] && [ -f "$SELF" ]; then
    local d
    d="$(cd "$(dirname "$SELF")" && pwd)"
    if [ -f "$d/main.go" ] && [ -f "$d/go.mod" ]; then
      SRC_DIR="$d"
      note "使用当前目录源码：$SRC_DIR"
    fi
  fi

  if [ -z "$SRC_DIR" ]; then
    ensure_git || warn "没有 git，改为直接下载源码包"
    if command -v git >/dev/null 2>&1; then
      if [ -d "$SRC_CACHE/.git" ]; then
        note "已有源码：$SRC_CACHE，更新到 $BRANCH"
        if git -C "$SRC_CACHE" fetch --depth 1 origin "$BRANCH" >/dev/null 2>&1 \
          && git -C "$SRC_CACHE" checkout -q FETCH_HEAD; then
          note "已更新到最新源码"
        else
          warn "更新失败（网络或分支问题），沿用现有源码"
        fi
      else
        note "克隆 $REPO_URL（分支 $BRANCH）"
        rm -rf "$SRC_CACHE"
        if ! git clone --depth 1 --branch "$BRANCH" "$REPO_URL" "$SRC_CACHE" 2>&1 | tail -3; then
          warn "git clone 失败，改下载源码包"
          download_source_tar
        fi
      fi
    else
      download_source_tar
    fi
    SRC_DIR="$SRC_CACHE/RekoboxForWindows/server"
  fi

  [ -f "$SRC_DIR/main.go" ] || die "源码不完整：$SRC_DIR 下没有 main.go"
  note "源码目录：$SRC_DIR"
}

# 产出 $BIN_TMP
build_binary() {
  step "编译"
  if [ -n "$BINARY" ]; then
    BIN_TMP="$BINARY"
    [ -s "$BIN_TMP" ] || die "找不到可用的二进制：$BIN_TMP"
    note "使用现成二进制：$BIN_TMP（$(du -h "$BIN_TMP" | cut -f1)）"
    return 0
  fi
  export CGO_ENABLED=0
  export GOFLAGS="-mod=mod"
  export GOPROXY="${GOPROXY:-https://goproxy.cn,direct}"
  export GOTOOLCHAIN=local
  note "GOPROXY=$GOPROXY"
  BIN_TMP="/tmp/rekosync.build.$$"
  BUILT_TMP=1
  if ! ( cd "$SRC_DIR" && go build -trimpath -ldflags "-s -w" -o "$BIN_TMP" . ); then
    rm -f "$BIN_TMP"
    die "编译失败。若在墙内：GOPROXY=https://goproxy.cn,direct bash deploy.sh"
  fi
  [ -s "$BIN_TMP" ] || die "编译产物为空"
  note "产物大小：$(du -h "$BIN_TMP" | cut -f1)"
}

# ---------- 服务 ----------
write_unit() {
  local listen="$1"
  mkdir -p /etc/systemd/system
  cat > "$UNIT_PATH" <<UNIT
[Unit]
Description=RekoBox node pool server (rekosync)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=rekosync
Group=rekosync
WorkingDirectory=${INSTALL_DIR}
ExecStart=${INSTALL_DIR}/rekosync -addr ${listen} -data ${INSTALL_DIR}/data
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
}

ensure_service_user() {
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
    note "已创建系统用户 rekosync"
  fi
}

install_binary() {
  mkdir -p "$INSTALL_DIR/data"
  # 覆盖前留一份，启动失败可回滚
  if [ -f "$INSTALL_DIR/rekosync" ]; then
    cp -f "$INSTALL_DIR/rekosync" "$INSTALL_DIR/rekosync.prev" 2>/dev/null || true
  fi
  install -m 0755 "$BIN_TMP" "$INSTALL_DIR/rekosync"
  # 只清理自己编译出来的临时产物，--binary 传进来的是用户文件，不能删
  if [ "$BUILT_TMP" = 1 ]; then rm -f "$BIN_TMP"; fi
  note "二进制：$INSTALL_DIR/rekosync"
}

write_conf() {
  cat > "$INSTALL_DIR/install.conf" <<CONF
# 由 deploy.sh 生成（更新/卸载时读取），删除本文件不影响服务运行
CONF_TOOL_VERSION=${TOOL_VERSION}
CONF_PORT=${SERVICE_PORT}
CONF_LISTEN=${LISTEN}
CONF_DIR=${INSTALL_DIR}
CONF_ACCOUNT=${ACCOUNT}
CONF_INSTALLED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
CONF
  chmod 600 "$INSTALL_DIR/install.conf" 2>/dev/null || true
  mkdir -p /etc
  cat > "$POINTER_FILE" <<CONF
# 记录 rekosync 的安装位置，供 deploy.sh 更新/卸载时定位
CONF_DIR=${INSTALL_DIR}
CONF
  chmod 600 "$POINTER_FILE" 2>/dev/null || true
}

firewall_open() {
  local port="$1"
  if [ -z "$WITH_FIREWALL" ]; then
    ask_yn "放行防火墙端口 ${port}/tcp" "y" WITH_FIREWALL
  fi
  [ "$WITH_FIREWALL" = "y" ] || { note "未改防火墙"; return 0; }
  if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -qi "Status: active"; then
    ufw allow "${port}/tcp" >/dev/null 2>&1 && note "ufw 已放行 ${port}/tcp" || warn "ufw 放行失败"
  elif command -v firewall-cmd >/dev/null 2>&1 && systemctl is-active --quiet firewalld; then
    firewall-cmd --permanent --add-port="${port}/tcp" >/dev/null 2>&1 \
      && firewall-cmd --reload >/dev/null 2>&1 \
      && note "firewalld 已放行 ${port}/tcp" || warn "firewalld 放行失败"
  else
    note "未检测到启用的 ufw/firewalld，跳过"
  fi
}

firewall_close() {
  local port="$1"
  [ -n "$port" ] || return 0
  if command -v ufw >/dev/null 2>&1; then
    ufw delete allow "${port}/tcp" >/dev/null 2>&1 && note "ufw 已移除 ${port}/tcp" || true
  fi
  if command -v firewall-cmd >/dev/null 2>&1 && systemctl is-active --quiet firewalld 2>/dev/null; then
    firewall-cmd --permanent --remove-port="${port}/tcp" >/dev/null 2>&1 || true
    firewall-cmd --reload >/dev/null 2>&1 || true
    note "firewalld 已移除 ${port}/tcp"
  fi
}

health_check() {
  local port="$1" body="" i
  for i in 1 2 3 4 5; do
    if body="$(curl -fsS --max-time 3 "http://127.0.0.1:${port}/api/v1/health" 2>/dev/null)"; then
      say "健康检查通过：$body"
      return 0
    fi
    sleep 1
  done
  warn "健康检查未通过，看日志：journalctl -u rekosync -n 50 --no-pager"
  return 1
}

public_ip() {
  local ip
  ip="$(curl -fsS --max-time 3 https://api.ipify.org 2>/dev/null || curl -fsS --max-time 3 https://ipv4.icanhazip.com 2>/dev/null || true)"
  printf '%s' "${ip:-<服务器公网IP>}"
}

# ---------- 动作：安装 ----------
do_install() {
  load_conf
  if is_installed && [ "$FORCE" = 0 ]; then
    warn "检测到已安装（$INSTALL_DIR）"
    if [ "$ASSUME_YES" = 1 ] || ! have_tty; then
      die "已安装。更新用 --update；覆盖安装用 --install --force；彻底重来用 --uninstall"
    fi
    local go_on=""
    ask_yn "继续覆盖安装（保留账号与节点池）" "n" go_on
    if [ "$go_on" != "y" ]; then
      note "已取消。更新：--update；卸载：--uninstall"
      exit 0
    fi
  fi

  # ---- 交互参数 ----
  if have_tty && [ "$ASSUME_YES" = 0 ]; then
    echo
    say "安装参数（直接回车用默认值）"
    if [ "$LISTEN_SET" = 0 ] && [ "$PORT_SET" = 0 ]; then
      local INPUT_PORT
      while :; do
        INPUT_PORT=""
        printf '服务端口（1-65535）[%s]: ' "$SERVICE_PORT" >&2
        IFS= read -r INPUT_PORT < "$TTY" || true
        INPUT_PORT="${INPUT_PORT:-$SERVICE_PORT}"
        if valid_port "$INPUT_PORT"; then SERVICE_PORT="$INPUT_PORT"; break; fi
        warn "端口要在 1-65535 之间"
      done
    fi
    if [ "$LISTEN_SET" = 0 ]; then
      local bind_local=""
      ask_yn "只监听本机 127.0.0.1（前置 nginx/CDN 时选 y）" "n" bind_local
      if [ "$bind_local" = "y" ]; then LISTEN="127.0.0.1:${SERVICE_PORT}"; else LISTEN=":${SERVICE_PORT}"; fi
      LISTEN_SET=1
    fi
    local dflt_dir="$INSTALL_DIR"
    ask "安装目录" "$dflt_dir" INSTALL_DIR
    if [ "$CREATE_ACCOUNT" = 1 ]; then
      ask "管理员账号（登录网页控制台）" "$ACCOUNT" ACCOUNT
      if [ "$PASSWORD_SET" = 0 ]; then
        local INPUT_PASS=""
        printf '管理员口令（至少 6 位，直接回车自动生成）: ' >&2
        IFS= read -r INPUT_PASS < "$TTY" || true
        if [ -n "$INPUT_PASS" ]; then PASSWORD="$INPUT_PASS"; PASSWORD_SET=1; fi
      fi
    fi
  fi
  if [ "$LISTEN_SET" = 0 ]; then
    valid_port "$SERVICE_PORT" || die "端口不合法：$SERVICE_PORT（应为 1-65535 的整数）"
    LISTEN=":${SERVICE_PORT}"
  fi
  SERVICE_PORT="${LISTEN##*:}"
  valid_port "$SERVICE_PORT" || die "端口不合法：$LISTEN"

  echo
  say "RekoBox 节点池服务 · 安装"
  note "工具版本：$TOOL_VERSION"
  note "系统：$OS_NAME ($ARCH_RAW)"
  note "安装目录：$INSTALL_DIR    监听：$LISTEN"

  # 1 依赖
  step "1/6 系统依赖"
  if [ -n "$BINARY" ]; then note "安装现成二进制，跳过"
  elif [ "$SKIP_DEPS" = 1 ]; then note "按要求跳过"
  else ensure_basics; fi
  command -v curl >/dev/null 2>&1 || die "缺少 curl，无法继续"

  # 2 Go
  step "2/6 Go 工具链"
  if [ -n "$BINARY" ]; then note "安装现成二进制，不需要 Go"
  elif go_ok; then note "已安装 $(go env GOVERSION)"
  elif [ "$SKIP_DEPS" = 1 ]; then die "--skip-deps 指定跳过，但当前没有 Go >= ${GO_MIN_MAJOR}.${GO_MIN_MINOR}"
  else
    note "未检测到 Go >= ${GO_MIN_MAJOR}.${GO_MIN_MINOR}，安装 go${GO_VERSION}.linux-${GO_ARCH}"
    install_go
    go_ok || die "Go 安装后仍不可用：$(command -v go || echo 未找到)"
  fi

  # 3 源码 / 4 编译
  fetch_source
  build_binary

  # 5 安装
  step "5/6 安装服务"
  if [ "$CREATE_ACCOUNT" = 1 ] && [ "$PASSWORD_SET" = 0 ] && [ -z "$PASSWORD" ]; then
    PASSWORD="$(rand_pass)"; GEN_PASS=1
  fi
  if [ "$CREATE_ACCOUNT" = 1 ] && [ "${#PASSWORD}" -lt 6 ]; then die "口令至少 6 位"; fi

  ensure_service_user
  install_binary
  write_conf
  write_unit "$LISTEN"

  if [ "$CREATE_ACCOUNT" = 1 ]; then
    if [ "$RESET_PASSWORD" = 1 ]; then
      if printf '%s\n' "$PASSWORD" | "$INSTALL_DIR/rekosync" -data "$INSTALL_DIR/data" -passwd "$ACCOUNT" >/dev/null 2>&1; then
        note "已重置账号 $ACCOUNT 的口令（旧令牌已吊销）"
      else
        warn "重置口令失败（账号可能不存在，下面会尝试创建）"
      fi
    fi
    local ADD_RC=0 ADD_OUT=""
    ADD_OUT="$(printf '%s\n' "$PASSWORD" | "$INSTALL_DIR/rekosync" -data "$INSTALL_DIR/data" -addadmin "$ACCOUNT" 2>&1)" || ADD_RC=$?
    if [ "$ADD_RC" = 0 ]; then
      note "已创建管理员账号 $ACCOUNT"
    elif printf '%s' "$ADD_OUT" | grep -qiE "exist|已存在"; then
      note "账号 $ACCOUNT 已存在，跳过（改口令加 --reset-password）"
      GEN_PASS=0
    else
      warn "建号失败：$(printf '%s' "$ADD_OUT" | tail -1)"
    fi
  fi

  chown -R rekosync:rekosync "$INSTALL_DIR" 2>/dev/null || true
  chmod 700 "$INSTALL_DIR/data"

  systemctl daemon-reload
  systemctl enable rekosync >/dev/null 2>&1 || true
  systemctl restart rekosync
  sleep 1
  if ! systemctl is-active --quiet rekosync; then
    journalctl -u rekosync -n 30 --no-pager || true
    die "服务启动失败（见上面日志）"
  fi
  note "systemd 状态：active"

  # 6 防火墙 + 自检
  step "6/6 防火墙与自检"
  firewall_open "$SERVICE_PORT"
  health_check "$SERVICE_PORT" || true

  local ip
  ip="$(public_ip)"
  cat <<EOF

──────────────── 安装完成 ────────────────
  服务程序  $(installed_version)
  服务      systemctl status rekosync --no-pager
  日志      journalctl -u rekosync -f
  网页控制台  http://$ip:$SERVICE_PORT/admin
  数据      $INSTALL_DIR/data（账号/令牌/节点池，备份就 tar 这个目录）
  配置文件   $INSTALL_DIR/install.conf
  客户端填  http://$ip:$SERVICE_PORT
EOF
  if [ "$CREATE_ACCOUNT" = 1 ]; then
    echo "  账号      $ACCOUNT"
    if [ "$GEN_PASS" = 1 ]; then
      printf '  口令      %s%s%s  %s(只显示这一次，请立刻保存)%s\n' "$C_OK" "$PASSWORD" "$C_END" "$C_WARN" "$C_END"
    elif [ "$PASSWORD_SET" = 1 ]; then
      echo "  口令      你刚才输入的那一个"
    else
      echo "  口令      账号已存在，沿用原口令"
    fi
  fi
  echo
  note "更新：bash deploy.sh --update    卸载：bash deploy.sh --uninstall"
  warn "HTTP 明文：口令与节点链接在链路上可见。公网长期使用请前置 nginx + TLS。"
  echo
}

# ---------- 动作：更新 ----------
do_update() {
  load_conf
  if ! is_installed; then
    die "没有检测到安装（$INSTALL_DIR）。先安装：bash deploy.sh --install"
  fi
  if [ "$LISTEN_SET" = 0 ]; then
    detect_port_from_unit
    LISTEN=":${SERVICE_PORT}"
  fi

  local old_ver
  old_ver="$(installed_version)"

  echo
  say "RekoBox 节点池服务 · 更新"
  note "工具版本：$TOOL_VERSION"
  note "安装目录：$INSTALL_DIR    监听：$LISTEN"
  note "当前版本：${old_ver:-未知}"

  if [ -z "$BINARY" ]; then
    if [ "$SKIP_DEPS" = 0 ]; then ensure_basics; fi
    if ! go_ok; then
      if [ "$SKIP_DEPS" = 1 ]; then die "--skip-deps 指定跳过，但没有 Go >= ${GO_MIN_MAJOR}.${GO_MIN_MINOR}"; fi
      note "未检测到 Go，安装 go${GO_VERSION}"
      install_go
    else
      note "Go：$(go env GOVERSION)"
    fi
    fetch_source
    build_binary
  else
    build_binary
  fi

  step "安装新版本"
  install_binary
  write_conf
  write_unit "$LISTEN"
  chown -R rekosync:rekosync "$INSTALL_DIR" 2>/dev/null || true
  chmod 700 "$INSTALL_DIR/data"

  systemctl daemon-reload
  systemctl restart rekosync
  sleep 1
  if ! systemctl is-active --quiet rekosync; then
    warn "新版本启动失败，尝试回滚"
    if [ -f "$INSTALL_DIR/rekosync.prev" ]; then
      mv -f "$INSTALL_DIR/rekosync.prev" "$INSTALL_DIR/rekosync"
      systemctl restart rekosync || true
      journalctl -u rekosync -n 30 --no-pager || true
      die "已回滚到上一个版本，请检查日志"
    fi
    journalctl -u rekosync -n 30 --no-pager || true
    die "服务启动失败，且没有可回滚的备份"
  fi
  note "systemd 状态：active"
  health_check "$SERVICE_PORT" || true

  cat <<EOF

──────────────── 更新完成 ────────────────
  版本      ${old_ver:-未知} → $(installed_version)
  服务      systemctl status rekosync --no-pager
  数据      未改动（$INSTALL_DIR/data），账号与节点池保留
  回滚      旧二进制留在 $INSTALL_DIR/rekosync.prev
EOF
  echo
}

# ---------- 动作：卸载 ----------
do_uninstall() {
  load_conf
  if ! is_installed && [ ! -d "$INSTALL_DIR" ] && [ ! -d "$SRC_CACHE" ] && [ ! -f "$POINTER_FILE" ]; then
    say "没有检测到安装，系统已经是干净状态"
    return 0
  fi
  if [ "$LISTEN_SET" = 0 ]; then detect_port_from_unit; fi

  echo
  say "RekoBox 节点池服务 · 完全卸载"
  note "将删除："
  note "  · systemd 服务 rekosync（停止 + 取消开机自启 + 删单元文件）"
  note "  · 程序与数据：$INSTALL_DIR"
  note "  · 源码缓存：$SRC_CACHE"
  note "  · 系统用户与用户组 rekosync"
  note "  · 防火墙放行规则（端口 ${SERVICE_PORT}，若存在）"
  echo
  if [ "$KEEP_DATA" = 1 ]; then
    warn "--keep-data：数据会先打包到 /var/backups 再删除"
  else
    warn "数据目录会被删除，账号/令牌/节点池无法恢复。要保留请加 --keep-data"
  fi

  if [ "$ASSUME_YES" = 0 ]; then
    confirm_word "确认彻底卸载" "uninstall"
  fi

  step "1/4 停止并移除服务"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl stop rekosync 2>/dev/null || true
    systemctl disable rekosync >/dev/null 2>&1 || true
    systemctl reset-failed rekosync >/dev/null 2>&1 || true
    rm -f "$UNIT_PATH"
    systemctl daemon-reload || true
    note "已移除 $UNIT_PATH"
  fi
  pkill -f "$INSTALL_DIR/rekosync" 2>/dev/null || true

  step "2/4 删除程序与数据"
  if [ "$KEEP_DATA" = 1 ] && [ -d "$INSTALL_DIR/data" ]; then
    local keep
    keep="/var/backups/rekosync-data-$(date +%Y%m%d-%H%M%S).tar.gz"
    mkdir -p /var/backups
    tar -czf "$keep" -C "$INSTALL_DIR" data 2>/dev/null || warn "备份失败"
    rm -rf "$INSTALL_DIR"
    note "数据已备份到 $keep，其余已删除"
  else
    rm -rf "$INSTALL_DIR"
    note "已删除 $INSTALL_DIR"
  fi
  rm -rf "$SRC_CACHE"
  note "已删除 $SRC_CACHE"
  rm -f /usr/local/bin/rekosync "$POINTER_FILE" 2>/dev/null || true
  rm -rf /tmp/rekosync.build.* /tmp/rekobox-src.tar.gz 2>/dev/null || true

  step "3/4 移除系统用户"
  if id -u rekosync >/dev/null 2>&1; then
    if command -v userdel >/dev/null 2>&1; then
      userdel rekosync 2>/dev/null || warn "userdel 失败（可能仍有进程占用）"
    else
      deluser rekosync >/dev/null 2>&1 || warn "deluser 失败"
    fi
    note "用户 rekosync 已移除"
  fi
  if getent group rekosync >/dev/null 2>&1; then
    if command -v groupdel >/dev/null 2>&1; then
      groupdel rekosync 2>/dev/null || warn "groupdel 失败"
    else
      delgroup rekosync >/dev/null 2>&1 || true
    fi
    note "用户组 rekosync 已移除"
  fi

  step "4/4 清理防火墙规则"
  firewall_close "$SERVICE_PORT"

  # 残留校验：确认可以干净重装
  local left=""
  [ -e "$INSTALL_DIR" ] && left="$left $INSTALL_DIR"
  [ -e "$SRC_CACHE" ] && left="$left $SRC_CACHE"
  [ -e "$UNIT_PATH" ] && left="$left $UNIT_PATH"
  [ -e "$POINTER_FILE" ] && left="$left $POINTER_FILE"
  if id -u rekosync >/dev/null 2>&1; then left="$left 用户:rekosync"; fi
  if command -v systemctl >/dev/null 2>&1 && systemctl list-unit-files 2>/dev/null | grep -q '^rekosync\.service'; then
    left="$left 单元:rekosync.service"
  fi

  echo
  if [ -n "$left" ]; then
    warn "仍有残留：$left"
    note "手动删除后再试，或检查是否有进程占用"
    exit 1
  fi
  say "卸载完成，系统已恢复干净状态"
  note "重新安装：bash deploy.sh --install"
  echo
}

# ---------- 菜单 ----------
show_menu() {
  local st ver
  load_conf   # 有指针文件时自动定位自定义安装目录
  st="$(service_state)"
  ver="$(installed_version)"

  echo
  printf '%s  RekoBox 服务端部署工具  v%s%s\n' "$C_B" "$TOOL_VERSION" "$C_END"
  printf '%s  ──────────────────────────────────────%s\n' "$C_DIM" "$C_END"
  note "系统：$OS_NAME ($ARCH_RAW)"
  if is_installed; then
    note "状态：$st    端口：$SERVICE_PORT    版本：${ver:-未知}"
    note "目录：$INSTALL_DIR"
  else
    note "状态：未安装"
  fi
  echo
  echo "  1) 安装"
  echo "  2) 更新到最新版本"
  echo "  3) 清理卸载（完全删除，可重新安装）"
  echo "  0) 退出"
  echo
}

main_menu() {
  show_menu
  local choice=""
  printf '请选择 [1/2/3/0]: ' >&2
  IFS= read -r choice < "$TTY" || true
  case "${choice:-}" in
    1) ACTION=install ;;
    2) ACTION=update ;;
    3) ACTION=uninstall ;;
    0|q|Q|"") say "已退出"; exit 0 ;;
    *) die "无效选择：$choice" ;;
  esac
}

# ---------- 分发 ----------
if [ -z "$ACTION" ]; then
  if [ "$ASSUME_YES" = 1 ] || ! have_tty; then
    ACTION=install          # 无终端 / -y：默认安装
  else
    main_menu
  fi
fi

case "$ACTION" in
  install)   do_install ;;
  update)    do_update ;;
  uninstall) do_uninstall ;;
  *) die "未知动作：$ACTION" ;;
esac
