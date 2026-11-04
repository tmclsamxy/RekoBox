# Rekobox for macOS

规划中。

## 计划

- 界面层参考 `RekoboxForWindows/`（Qt Widgets）：Qt 跨平台，但 `sys/windows/` 下的
  提权、注册表、WinINet 要换成本地实现，托盘走 `QSystemTrayIcon`
- 内核：`sing-box` 官方 macOS 二进制（arm64 + amd64），与 Windows 版一样走子进程 + Clash API
- 隧道：sing-box 自带 `tun` 入站 + utun，需要 `NetworkExtension` 或 root 权限；
  只做系统代理则不需要特权
- 系统代理：`networksetup -setwebproxy` / `-setsocksfirewallproxy`，退出时还原
- 打包：`jpackage` 生成 `.app`，再打 `.dmg`，需要 Apple 开发者 ID 签名才不触发 Gatekeeper

## 状态

尚未开始。等 Windows 版稳定后启动。
