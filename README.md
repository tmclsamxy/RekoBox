# Rekobox

一个跨平台的代理客户端，源自 [NekoBox for Android](https://github.com/MatsuriDayo/NekoBoxForAndroid)（GPL-3.0）。

> **刚拿到这份代码、准备在 Mac 上开发？** → [`START-HERE.md`](START-HERE.md)
> 开发 macOS 端的接手说明：[`RekoboxForMac/docs/HANDOFF.md`](RekoboxForMac/docs/HANDOFF.md)

```
Rekobox/
├── RekoboxForAndroid/     Android 版（Kotlin + gomobile 内核，包名 io.rekobox）
├── RekoboxForWindows/     Windows 版（nekoray 移植，Qt6 Widgets + MinGW）；同时是桌面端共享树
├── RekoboxForMac/         macOS 版（骨架，构建入口在共享树里）
├── docs/                  改名清单、移植方案、发布流程
├── sing-box/              内核源码（Android libcore 与桌面端 rekobox_core 共用，不入库）
└── libneko/               Go 辅助库（同上，不入库）
```

## 平台状态

| 平台 | 目录 | 当前状态 |
|---|---|---|
| **Android** | `RekoboxForAndroid/` | v1.0.1 已发布；`io.rekobox`，4 个 split ABI；改名基于 NekoBox |
| **Windows** | `RekoboxForWindows/` | nekoray 4.0.1 移植：Qt6.7 + MinGW 构建通过，与 Android 共用 sing-box 内核（含 VLESS Encryption / Vision），已实机运行 |
| **macOS** | `RekoboxForMac/` | Apple Silicon / arm64，**已在 M2（macOS 15.7.5）编译通过并启动**，界面一致；TUN / 系统代理待验证。方案 [`docs/06-macos-port.md`](docs/06-macos-port.md)，交接 [`RekoboxForMac/docs/HANDOFF.md`](RekoboxForMac/docs/HANDOFF.md)，效果图在 `RekoboxForMac/mockups/`。源码复用 `RekoboxForWindows/`（CMake 按平台分支，不复制副本） |

## 发布

仓库：https://github.com/tmclsamxy/RekoBox

| 平台 | 版本 | 产物 | 链接 |
|---|---|---|---|
| Android | v1.0.6 | 4 个 split APK | [releases/tag/v1.0.6](https://github.com/tmclsamxy/RekoBox/releases/tag/v1.0.6) |
| Windows | win-v1.0.15 | `Rekobox-1.0.15-windows-x64-qt.zip` + `rekosync` 服务端 | [releases/tag/win-v1.0.15](https://github.com/tmclsamxy/RekoBox/releases/tag/win-v1.0.15) |
| 服务端 | server-v2.1.0 | `rekosync` 单文件二进制（linux amd64/arm64/arm、windows amd64、darwin amd64/arm64）+ `SHA256SUMS` | [releases/tag/server-v2.1.0](https://github.com/tmclsamxy/RekoBox/releases/tag/server-v2.1.0) |

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

### macOS（Apple Silicon，进行中）

```bash
brew install cmake ninja go
python3 -m aqt install-qt mac desktop 6.7.2 clang_64 -O ~/Qt --archives qtbase qtsvg qttools

bash RekoboxForWindows/libs/build_deps_macos.sh   # 依赖（版本与 Windows 对齐）
bash RekoboxForWindows/libs/get_source_local.sh   # 内核源码
bash RekoboxForWindows/libs/build_macos.sh        # → RekoboxForMac/build/RekoBox.app
```

第一次编译的预期问题与修法见 `RekoboxForMac/docs/HANDOFF.md` §4。

## 服务端（可选，一条命令）

节点池服务端 `rekosync` 跑在任意 Linux VPS 上：管理员在网页控制台粘贴分享链接维护节点、
用**权限组**决定每个账号能看到哪些节点，客户端登录后**只读**拉取，任何人（包括自己）
都无法上传或改动别人的节点。

```bash
curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh | sudo bash
# 交互提示没出现时（个别终端对管道有限制）：
sudo bash <(curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh)
```

脚本是交互菜单：

```
  RekoBox 服务端部署工具  v2.1.0
  ──────────────────────────────────────
    系统：Ubuntu 24.04 (x86_64)
    状态：未安装

  1) 安装
  2) 更新到最新版本
  3) 清理卸载（完全删除，可重新安装）
  0) 退出
```

- **安装**：逐项询问服务端口、是否只监听 `127.0.0.1`、安装目录、管理员账号与口令（留空自动生成）、
  是否放行防火墙；然后自动装依赖 / Go / 拉源码 / 编译 / 装 systemd / 建管理员 / 自检
- **更新**：拉最新源码重编译，`data/` 不动；启动失败自动回滚
- **卸载**：服务 + 程序 + 数据 + 源码缓存 + 系统用户 + 防火墙规则全清，残留校验，可立即重装

装完打开 `http://<服务器>:<端口>/admin` 用管理员账号登录即可录节点。
详见 [`RekoboxForWindows/server/README.md`](RekoboxForWindows/server/README.md) 与
[`docs/04-account-sync.md`](docs/04-account-sync.md)。

## 文档

| 文件 | 内容 |
|---|---|
| `docs/01-android-rebrand.md` | Android 改名清单：每个文件改哪一行、为什么 |
| `docs/02-windows-port.md` | Windows 移植方案：架构差异、目录结构、分阶段步骤 |
| `docs/03-release.md` | 发布流程与产物约定 |
| `docs/04-account-sync.md` | 账号与节点池：只读订阅模型、网页控制台、接口、权限、离线策略、服务端部署 |
| `docs/05-acl-permission-groups.md` | 权限组：账号/组/节点模型、迁移、按组鉴权、版本失效机制、测试与上线 |
| `docs/06-macos-port.md` | macOS 移植方案：界面一致性结论、共享树决策、M2 工具链、TUN 权限、签名公证、风险清单 |

## 许可

GPL-3.0，与上游 NekoBox for Android 一致。仓库公开，发布页附对应源码 tag。
