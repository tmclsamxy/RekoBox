# Rekobox for macOS

规划中。

## 计划

- 界面层直接复用 `RekoboxForWindows/app/src/main/kotlin/io/rekobox/win/ui/`
  （Compose Desktop 的 UI 代码跨平台，`MenuBar`/`Tray`/文件路径是少数需要分支的地方）
- 内核：`sing-box` 官方 macOS 二进制（arm64 + amd64），与 Windows 版一样走子进程 + Clash API
- 隧道：sing-box 自带 `tun` 入站 + utun，需要 `NetworkExtension` 或 root 权限；
  只做系统代理则不需要特权
- 系统代理：`networksetup -setwebproxy` / `-setsocksfirewallproxy`，退出时还原
- 打包：`jpackage` 生成 `.app`，再打 `.dmg`，需要 Apple 开发者 ID 签名才不触发 Gatekeeper

## 状态

尚未开始。等 Windows 版稳定后启动。
