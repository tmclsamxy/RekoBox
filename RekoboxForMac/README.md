# Rekobox for macOS

`RekoBox.app`（Apple Silicon / M2 起）—— 与 Windows 版同源的 Qt 6.7 Widgets 客户端。

## 状态

骨架已完成，**尚未在 Mac 上编译过**。接手请先看
[`docs/HANDOFF.md`](docs/HANDOFF.md)，方案全文在 [`../docs/06-macos-port.md`](../docs/06-macos-port.md)。

## 怎么构建（在 M2 上）

```bash
brew install cmake ninja go
python3 -m aqt install-qt mac desktop 6.7.2 clang_64 -O ~/Qt \
        --archives qtbase qtsvg qttools

bash RekoboxForWindows/libs/build_deps_macos.sh   # zxing / yaml / protobuf（版本对齐 Windows）
bash RekoboxForWindows/libs/get_source_local.sh   # 拉 sing-box 内核源码
bash RekoboxForWindows/libs/build_macos.sh        # 编 .app
bash RekoboxForWindows/libs/build_macos.sh --dmg  # 顺带出 dmg
```

产物：`build/RekoBox.app`。源码在 `../RekoboxForWindows/`（桌面共享树，不复制副本）。

## 目录

```
RekoboxForMac/
├── docs/HANDOFF.md        交接文档：现状 / 执行顺序 / 预期编译错误表
├── mockups/               界面效果图（index.html + 5 张 png + shoot.js）
├── build/                 构建产物（gitignore）
└── dist/                  dmg（gitignore）
```

## 结论摘要

- **界面**：直接用 `RekoboxForWindows/` 这套 Qt 源码编译，内容和 Windows 完全一致；
  只有菜单栏位置、窗口按钮、托盘位置、字体这 4 处由系统决定，消不掉也不需要消。
- **内核**：`sing-box 1.12.19-neko-1` 编成 `darwin/arm64`，仍走「子进程 + gRPC 遥控」。
- **系统代理**：`networksetup`（代码分支已存在），不需要管理员。
- **TUN**：`utun` + `auto_route`，需要 root；MVP 用 `osascript` 授权，P4 换 `SMAppService` 特权助手。
- **打包**：`macdeployqt` → `.app` → `.dmg`，Developer ID 签名 + `notarytool` 公证。
- **别走 Android 路线**：Fragment/View 体系，复用率约 0。

## 效果图

`mockups/index.html`（浏览器打开）。五张：直译版浅色/深色、Mac 化外壳、菜单栏面板、首选项。
