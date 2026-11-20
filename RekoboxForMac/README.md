# Rekobox for macOS

`RekoBox.app`（Apple Silicon / M2 起）—— 与 Windows 版同源的 Qt 6.7 Widgets 客户端。

## 状态

**已编译通过并启动**（2026-11-20，M2 / macOS 15.7.5 / Qt 6.7.2 / arm64），界面与效果图 A1 图一致。
TUN 与系统代理尚未实机验证。接手请先看
[`docs/HANDOFF.md`](docs/HANDOFF.md)，方案全文在 [`../docs/06-macos-port.md`](../docs/06-macos-port.md)。

## 怎么构建（在 M2 上）

```bash
brew install cmake ninja go            # 或不用 sudo 把 cmake/ninja/go 解压到 ~/tools，见 HANDOFF §3 Step 0
python3 -m aqt install-qt mac desktop 6.7.2 clang_64 -O ~/Qt \
        --archives qtbase qtsvg qttools

export GOPROXY=https://goproxy.cn,direct   # 国内拉 Go 模块，否则内核那步会卡很久
bash RekoboxForWindows/libs/build_deps_macos.sh   # zxing / yaml / protobuf（版本对齐 Windows）
bash RekoboxForWindows/libs/get_source_local.sh   # 拉 sing-box 内核源码
bash RekoboxForWindows/libs/build_macos.sh        # 编 .app
bash RekoboxForWindows/libs/build_macos.sh --dmg  # 顺带出 dmg
```

产物：`build/RekoBox.app`；加 `--dmg` 会在 `dist/` 出 `Rekobox-<版本>-mac-arm64.dmg`
（里面是 app + `/Applications` 快捷方式，拖进去就装好）。源码在 `../RekoboxForWindows/`
（桌面共享树，不复制副本）。

### 安装说明（重要）

产物是 **ad-hoc 签名**，没有 Developer ID、没公证。装好第一次打开要：

- 右键点 `RekoBox.app` → **打开** → 弹窗里再点"打开"；或
- 终端里 `xattr -dr com.apple.quarantine /Applications/RekoBox.app`

不这么做会提示"无法验证开发者"。它不会报"已损坏"——那说明签名坏了（比如 bundle 里
有断链符号链接），`codesign --verify --deep --strict RekoBox.app` 能查出来。

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
