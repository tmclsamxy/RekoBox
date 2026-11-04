# Rekobox

一个跨平台的代理客户端，源自 [NekoBox for Android](https://github.com/MatsuriDayo/NekoBoxForAndroid)（GPL-3.0）。

```
Rekobox/
├── RekoboxForAndroid/     Android 版（Kotlin + gomobile 内核，包名 io.rekobox）
├── RekoboxForWindows/     Windows 版（Compose Desktop + sing-box 子进程）
├── RekoboxForWindowsQt/   Windows Qt 版（nekoray 移植，Qt6 + MinGW）
├── RekoboxForMac/         macOS 版（规划中）
├── docs/                  改名清单、移植方案、发布流程
├── sing-box/              内核源码（Android libcore 的 go.mod replace 目标，不入库）
└── libneko/               Go 辅助库（同上，不入库）
```

## 平台状态

| 平台 | 目录 | 当前状态 |
|---|---|---|
| **Android** | `RekoboxForAndroid/` | v1.0.1 已发布；`io.rekobox`，4 个 split ABI；改名基于 NekoBox |
| **Windows** | `RekoboxForWindows/` | 可用闭环：9 种协议、订阅、节点库、托盘、系统代理 / TUN |
| **Windows (Qt)** | `RekoboxForWindowsQt/` | nekoray 4.0.1 移植：Qt6.7 + MinGW 构建通过，sing-box 1.9.7-neko 内核，已实机启动 |
| **macOS** | `RekoboxForMac/` | 规划中，界面层可复用 Windows 的 Compose 代码 |

## 架构差异（为什么不是一套代码编译两次）

| 关注点 | Android | Windows |
|---|---|---|
| 内核装载 | `libcore.aar`（gomobile），同进程 | `sing-box.exe` 子进程 |
| 隧道 | `VpnService` fd → hev-socks5-tunnel | 内核自建 `tun`（Wintun） |
| 控制面 | AIDL `ISagerNetService` | Clash API `127.0.0.1:9090` |
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

```powershell
cd RekoboxForWindows
powershell -ExecutionPolicy Bypass -File scripts\fetch-core.ps1   # 下载 sing-box + wintun（约 30 MB）
.\gradlew.bat :core:test                                          # 15 项单测，含 `sing-box check` 真校验
.\gradlew.bat :app:run                                            # 起界面
```

界面里粘贴 `ss://` / `vmess://` / `vless://` / `trojan://` / `hysteria2://` / `tuic://` / `anytls://` 链接，
或到「订阅」页加订阅，然后点「连接」。命令行验证：

```bash
curl --proxy http://127.0.0.1:2080 https://ip.sb
```

打包：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package.ps1     # 需要 WiX 3.x
```

## 文档

| 文件 | 内容 |
|---|---|
| `docs/01-android-rebrand.md` | Android 改名清单：每个文件改哪一行、为什么 |
| `docs/02-windows-port.md` | Windows 移植方案：架构差异、目录结构、分阶段步骤 |
| `docs/03-release.md` | 发布流程与产物约定 |

## 许可

GPL-3.0，与上游 NekoBox for Android 一致。仓库公开，发布页附对应源码 tag。
