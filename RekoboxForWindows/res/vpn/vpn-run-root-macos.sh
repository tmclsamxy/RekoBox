#!/bin/sh
# Rekobox for macOS —— TUN 模式启动脚本（以 root 运行）
#
# 与 Linux 版（vpn-run-root.sh）的区别：
#   - 不需要 iptables：sing-box 用 auto_route 自己写路由表
#   - 不需要预先建网卡：sing-box 直接开 utunN
#   - 不需要收尾清理：路由由 sing-box 退出时回收
#
# ---------------------------------------------------------------
# 停止握手（P0-3：不再二次弹密码）
#
# 原来停 TUN 是再跑一次 `osascript ... with administrator privileges` 去 pkill，
# 于是启停各弹一次密码；用户点"取消"就杀不掉 root 内核，utun 与 72 条路由残留，
# 表现为"关掉 TUN 反而整机断网"。
#
# 现在：本脚本以 root 起来之后，自己起一个哨兵循环（同为 root，所以能 kill 内核）。
# GUI 侧停止时只需要在 config 目录里**建一个普通文件**（用户对自己的目录有写权限，
# 不需要任何提权），哨兵看到就把内核 SIGTERM 掉并删掉该文件。
#
#   $CONFIG_DIR/tun-root.pid          内核 PID（root 写、用户可读）
#   $CONFIG_DIR/tun-stop-request      GUI 建的"请停"标记（root 看到即删）
#
# 只有哨兵已经不在（旧版本残留 / 被强杀）时，GUI 才回退到原来的 osascript 提权路径。
# ---------------------------------------------------------------
#
# 下面两处占位由 db/ConfigBuilder.cpp 的 WriteVPNLinuxScript() 在运行时替换：
#   内核相对路径  -> 内核绝对路径
#   配置变量      -> 生成的 tun 配置绝对路径

if [ "$EUID" -ne 0 ]; then
  echo "[Warning] tun script is not running as root"
fi

CORE_BIN=./rekobox_core
CONFIG_DIR=$(dirname "$CONFIG_PATH")
PID_FILE="$CONFIG_DIR/tun-root.pid"
STOP_FILE="$CONFIG_DIR/tun-stop-request"

# 只清 pid 残留。**不要**清 STOP_FILE：GUI 有可能在脚本还没跑到这里时就已经
# 投了"请停"（内核启动很快、用户手快连点勾选框），先删掉会把这个请求吃掉，
# 表现成"点了关闭没反应"。GUI 在启动前会自己清一次，所以不会误命中陈旧标记。
rm -f "$PID_FILE"

BASEDIR=$(dirname "$0")
cd "$BASEDIR" || exit 1

"$CORE_BIN" --disable-color run -c "$CONFIG_PATH" &
CORE_PID=$!
echo "$CORE_PID" > "$PID_FILE"
echo "[tun] core started, pid=$CORE_PID"

# 哨兵：内核活着就一直盯 stop-request；内核自己退了就收尾
#
# 用 kill -0 判活而不是 wait：wait 只能等我们自己 fork 的孩子，这里就是，
# 但 wait 期间没法轮询文件，所以两个都要——先轮询，内核退出后才 wait 回收。
#
# ⚠️ stop-request 里必须写着**本进程的 pid** 才算数。
# 只判文件存在是不够的：上一次崩溃留下的旧标记会让新内核刚起来就被自己杀掉
# （实测过：日志里 "core started" 紧接着 "stop requested"，TUN 根本起不来）。
# 校验 pid 之后，旧标记的内容对不上，自然被忽略。
while kill -0 "$CORE_PID" 2>/dev/null; do
  if [ -e "$STOP_FILE" ]; then
    REQ=$(cat "$STOP_FILE" 2>/dev/null)
    if [ "$REQ" = "$CORE_PID" ]; then
      echo "[tun] stop requested, sending SIGTERM to $CORE_PID"
      rm -f "$STOP_FILE"
      kill -TERM "$CORE_PID" 2>/dev/null
      break
    fi
    # 陈旧标记（内容不是本次的 pid）：删掉，继续跑
    rm -f "$STOP_FILE"
  fi
  sleep 0.3
done

# 给内核一点时间自己清理路由与 utun；不走就强杀，别让 GUI 干等
i=0
while kill -0 "$CORE_PID" 2>/dev/null; do
  i=$((i + 1))
  if [ "$i" -ge 20 ]; then
    echo "[tun] core did not exit in 6s, sending SIGKILL"
    kill -KILL "$CORE_PID" 2>/dev/null
    break
  fi
  sleep 0.3
done

wait "$CORE_PID" 2>/dev/null
rm -f "$PID_FILE" "$STOP_FILE"
echo "[tun] core exited"