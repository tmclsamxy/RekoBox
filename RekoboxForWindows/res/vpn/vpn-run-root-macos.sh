#!/bin/sh
# Rekobox for macOS —— TUN 模式启动脚本（以 root 运行）
#
# 与 Linux 版（vpn-run-root.sh）的区别：
#   - 不需要 iptables：sing-box 用 auto_route 自己写路由表
#   - 不需要预先建网卡：sing-box 直接开 utunN
#   - 不需要收尾清理：路由由 sing-box 退出时回收
#
# 下面两处占位由 db/ConfigBuilder.cpp 的 WriteVPNLinuxScript() 在运行时替换：
#   内核相对路径  -> 内核绝对路径
#   配置变量      -> 生成的 tun 配置绝对路径

set -x

if [ "$EUID" -ne 0 ]; then
  echo "[Warning] tun script is not running as root"
fi

BASEDIR=$(dirname "$0")
cd "$BASEDIR" || exit 1

exec ./rekobox_core --disable-color run -c "$CONFIG_PATH"
