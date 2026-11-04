# Rekobox

一个跨平台的代理客户端，源自 [NekoBox for Android](https://github.com/MatsuriDayo/NekoBoxForAndroid)（GPL-3.0）。

```
Rekobox/
├── RekoboxForAndroid/     Android 版（Kotlin + gomobile 内核，包名 io.rekobox）
├── RekoboxForWindows/     Windows 版（nekoray 移植，Qt6 Widgets + MinGW）
├── RekoboxForMac/         macOS 版（规划中）
├── docs/                  改名清单、移植方案、发布流程
├── sing-box/              内核源码（Android libcore 的 go.mod replace 目标，不入库）
└── libneko/               Go 辅助库（同上，不入库）
```

## 平台状态

| 平台 | 目录 | 当前状态 |
|---|---|---|
| **Android** | `RekoboxForAndroid/` | v1.0.1 已发布；`io.rekobox`，4 个 split ABI；改名基于 NekoBox |
| **Windows** | `RekoboxForWindows/` | nekoray 4.0.1 移植：Qt6.7 + MinGW 构建通过，sing-box 1.12.19-neko 内核，已实机运行 |
| **macOS** | `RekoboxForMac/` | 规划中，界面层参考 Windows 的 Qt 实现 |

## 发布

仓库：https://github.com/tmclsamxy/RekoBox

| 平台 | 版本 | 产物 | 链接 |
|---|---|---|---|
| Android | v1.0.1 | 4 个 split APK | [releases/tag/v1.0.1](https://github.com/tmclsamxy/RekoBox/releases/tag/v1.0.1) |
| Windows | win-v1.0.2 | `Rekobox-1.0.2-windows-x64-qt.zip` | [releases/tag/win-v1.0.2](https://github.com/tmclsamxy/RekoBox/releases/tag/win-v1.0.2) |

## 架构差异（为什么不是一套代码编译两次）

| 关注点 | Android | Windows |
|---|---|---|
| 内核装载 | `libcore.aar`（gomobile），同进程 | `sing-box.exe` 子进程 |
| 隧道 | `VpnService` fd → hev-socks5-tunnel | 内核自建 `tun`（Wintun） |
| 控制面 | AIDL `ISagerNetService` | gRPC（protobuf over HTTP/2，手写客户端） |
| 分应用 | VpnService UID 白名单 | 路由规则 `process_name` |
| 权限 | VPN 授权弹窗 | UAC（仅 TUN 需要） |

`gomobile bind` 没有 windows target，且隧道建立方向相反（Android 由 App 给核心 fd，
Windows 必须核心自己建网卡），所以 Windows 走「核心独立进程 + 图形界面遥控」这条路。

## 快速开始

### Android

```bash
cd RekoboxForAndroid
bash tools/rebrand.sh                  # 幂等；插件协议字面量已做冻结保护
./gradlew app:assembleOssDebug         # 需要 NDK + Go + gomobile
```

### Windows

前置：Qt 6.7.2 mingw_64 + MinGW 13.1（`D:/Qt`）、CMake 3.16+、Ninja、Go 1.25+。

```bash
cd RekoboxForWindows
bash libs/build_all_mingw.sh          # 依赖 → 内核 → CMake → 编译 → 部署，首次约 15 分钟
```

产物在 `deployment/windows64/`，运行里面的 `rekobox.exe` 即可（Qt 运行时已随包，无需另装）。
装 Qt 与工具链的完整命令见 `RekoboxForWindows/README.md`。

命令行验证代理是否生效：

```bash
curl --proxy http://127.0.0.1:2080 https://ip.sb
```

## 文档

| 文件 | 内容 |
|---|---|
| `docs/01-android-rebrand.md` | Android 改名清单：每个文件改哪一行、为什么 |
| `docs/02-windows-port.md` | Windows 移植方案：架构差异、目录结构、分阶段步骤 |
| `docs/03-release.md` | 发布流程与产物约定 |

## 许可

GPL-3.0，与上游 NekoBox for Android 一致。仓库公开，发布页附对应源码 tag。
