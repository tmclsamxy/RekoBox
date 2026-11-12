# RekoboxForWindows

基于 [MatsuriDayo/nekoray](https://github.com/MatsuriDayo/nekoray) `4.0.1-2024-12-12` 的 Windows Qt 客户端，
已改名为 **RekoBox** 并在 Windows 上完成原生构建与运行验证。

> 本目录即 RekoBox 的 Windows 实现（源于 nekoray），保留 Qt Widgets 界面与 gRPC 架构，
> 改动集中在工具链、构建系统与依赖。

---

## 1. 现状

| 项 | 值 |
|---|---|
| 界面 | Qt 6.7.2 Widgets（MinGW 13.1 构建） |
| 内核 | `rekobox_core.exe` = sing-box **1.12.19-neko-1** + libneko |
| 编译器 | MinGW-w64 GCC 13.1（原工程 CI 用 MSVC 14.2） |
| 构建 | CMake 3.31 + Ninja |
| 产物 | `deployment/windows64/`（约 95 MB），打包 `dist/Rekobox-1.0.4-windows-x64-qt.zip`（32 MB） |
| 验证 | GUI 离屏启动 10 s 无崩溃；内核打印版本并进入 gRPC 等待态 |

---

## 2. 关键改动

### 2.1 工具链与构建系统

| 改动 | 原状 | 现状 | 原因 |
|---|---|---|---|
| C++ 编译器 | MSVC 14.2（CI 专用） | MinGW-w64 GCC 13.1 | 本机无管理员权限装不了 VS Build Tools；MinGW 随 Qt 分发 |
| Qt 版本 | 默认 Qt5，CI 用 Qt 6.7 MSVC 包 | Qt 6.7.2 MinGW 包 | Qt5 官方已无 Windows 二进制；MSVC 包配不了 MinGW |
| `cmake_minimum_required` | 3.5 | 3.16 | CMake 4.x 拒绝 `<3.5` 的工程；3.16 同时兼容 3.31 |
| `QT_VERSION_MAJOR` 默认值 | 5 | 6 | 免去每次传 `-D` |
| 目标名 | `nekobox` | `rekobox` | 品牌统一，产物 `rekobox.exe` |
| Windows 宏 | 仅 MSVC 分支定义 `_WIN32_WINNT` | MinGW 分支也定义 `0x0600` | `rasapi32` / `iphlpapi` 头文件依赖 |

新增脚本：

- `libs/build_deps_mingw.sh` —— 用 MinGW 从源码编译 zxing-cpp 2.0 / yaml-cpp 0.7 / protobuf 21.4
- `libs/deploy_windows_mingw.sh` —— `windeployqt` + MinGW 运行时 + 分流数据 + wintun
- `libs/build_all_mingw.sh` —— 一键：依赖 → 内核 → CMake → 编译 → 部署

### 2.2 依赖

| 依赖 | 版本 | 说明 |
|---|---|---|
| zxing-cpp | 2.0.0 | 扫码，静态链接 |
| yaml-cpp | 0.7.0 | 配置解析，静态链接 |
| protobuf | 21.4 | 仅 libprotobuf + protoc 生成 RPC 消息体 |
| QHotkey | master | 全局快捷键，随源码编入 |
| Qt | 6.7.2 | Widgets / Network / Svg / LinguistTools |

> 这里的“gRPC”不是 gRPC C++ 库。`rpc/gRPC.cpp` 是**手写的 gRPC-over-HTTP2 客户端**，
> 只用 QtNetwork + protobuf，不需要 abseil / re2 / c-ares —— 这是 MinGW 路线能走通的关键。

### 2.3 Go 内核

- 三方源码改为**仓库内自带**（`_deps/sing-box`、`_deps/sing-quic`、`_deps/libneko`），
  不再依赖 `../sing-box` 这类仓库外相对路径，避免与 Android 端内核源码互相覆盖
- `go.mod` 的 `replace` 指向 `../../../_deps/*`
- Go 1.25.4 构建（工程仍声明 `go 1.19`，不改模块声明以免触发依赖重解析）
- 编译标签：`with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls,with_ech`
- 产物命名 `rekobox_core.exe`，`NekoGui.cpp` 的查找名已同步

### 2.4 品牌

| 位置 | 原 | 现 |
|---|---|---|
| 目标 / 产物 | `nekobox` | `rekobox.exe` |
| 显示名 `software_name` | `NekoBox` | `RekoBox` |
| 配置文件 | `groups/nekobox.json` | `groups/rekobox.json` |
| 单实例锁 | `nekoray<path>` | `rekobox<path>` |
| 本地 socket 前缀 | `nekoraylocalserver-` | `rekoboxlocalserver-` |
| 图标 | `res/nekobox.ico` | `res/rekobox.ico` |
| 托盘图 | `public/nekobox.png` | `public/rekobox.png` |
| 翻译 | `translations/*.ts` 内产品名 | 同步替换 |

---

## 3. 构建

前置（一次性）：

```bash
# Qt + MinGW（约 1.2 GB）
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.7.2 win64_mingw -O D:/Qt --archives qtbase qtsvg qttools icu
python -m aqt install-tool windows desktop tools_mingw1310 -O D:/Qt

# 构建工具
python -m pip install "cmake==3.31.6" ninja
```

然后：

```bash
cd RekoboxForWindows
bash libs/build_all_mingw.sh        # 首次约 15 分钟
```

产物：`deployment/windows64/rekobox.exe`，直接双击运行，无需另装 Qt。

---

## 4. 内核升级（1.9.7-neko → 1.12.19-neko-1）

上游 1.11+ 删掉了 nekoray 依赖的 `cmd/sing-box` 可导入入口（改回 `package main`），
所以不能只改 commit。做法是在 sing-box 源码树里新加一个公开包 `neko/`，用 sing-box 的公开 API
重新实现原来那三件事：

| 原实现（1.9.7） | 现在（1.12） |
|---|---|
| `boxmain.Create()`（`cmd/sing-box` 内的补丁） | `neko.Create()`：`include.Context()` + `json.UnmarshalExtendedContext[option.Options]` + `box.New` |
| `boxmain.SetDisableColor()` | `neko.SetDisableColor()`，写进 `options.Log.DisableColor` |
| `instance.SetLogWritter(w)` | `box.Options.PlatformLogWriter`（实现 `log.PlatformWriter`），`neko.Writer` 是适配器 |
| `Router().SetV2RayServer(boxapi.NewSbV2rayServer(...))` | 写进 `options.Experimental.V2RayAPI.Stats`，查询走 `v2rayapi.StatsService.GetStats()`（`neko.QueryStats`） |
| `boxmain.Main()`（sing-box CLI） | `neko.CLI()`：只实现 `run -c` / `check -c` / `version`（GUI 的 TUN 模式只用到这几个） |
| `boxapi.DialContext(ctx, box, network, addr)` | 多一个 `tracker` 参数，传 `nil` 即可 |
| `boxapi.CreateProxyHttpClient(box)` | 同上，多一个 `tracker` 参数 |
| `boxapi.DialUDP` | 已移除；libneko 里本来就只是声明未使用，直接删掉该 hook |

依赖侧还有两处改动：

- 去掉 `replace github.com/sagernet/sing-quic => _deps/sing-quic`：旧副本的
  `quic.ConnectionState` 没有 `ExportKeyingMaterial`，1.12 要配套的 quic-go
- 编译标签去掉 `with_ech`：1.12 起 ECH 并入 stdlib，该标签会触发一个故意报错的占位文件

验证：

```bash
rekobox_core.exe version          # sing-box version 1.12.19-neko-1
rekobox_core.exe check -c cfg.json # 旧版 DNS 写法（servers[].address + detour）仍可被自动迁移
rekobox_core.exe --disable-color run -c cfg.json   # mixed 入站正常 LISTENING，日志无重复行
```

## 5. 账号与云端同步（可选）

登录后把分组与节点同步到自建服务器，多设备保持一致；不登录照常本地使用。
完整设计见 [`../docs/04-account-sync.md`](../docs/04-account-sync.md)。

| 位置 | 内容 |
|---|---|
| `sync/` | 客户端同步层：`SyncModel` 合并算法 / `SyncStore` 本地状态 / `SyncApi` HTTP / `SyncEngine` 状态机 |
| `server/` | 服务端 `rekosync`（Go 标准库，单文件，无数据库），含 systemd unit |
| `server/deploy.sh` | **服务器上**一键安装：装依赖与 Go → 拉源码 → 编译 → systemd → 建号 → 自检 |
| `server/deploy-remote.sh` | **开发机上**远程部署：交叉编译后上传，或交给服务器自行编译 |
| `ui/dialog_account.*` | 首选项 →「账号与同步」对话框 |
| `libs/build_linux_sync.sh` | 交叉编译 Linux 服务端（远程部署与 release 用） |

要点：

- 账号**不开放注册**，由服务器管理员 `rekosync -adduser <name>` 创建。
- 令牌**永久有效**（`expires_at=0`），只有主动注销、服务端改口令或删号才失效。
- 同步以 UUID（实体 `sync_id`）为条目标识，删除用墓碑，
  冲突按条目 mtime 做 last-write-wins，服务端只用 `version` 做乐观锁（409 → 重新合并）。
- 断网时全部改动落本地（`config/sync/state.json` 的 `pending`），
  联网后按 5s→10s→…→5min 退避自动补推。
- 默认**不走代理**同步（服务端通常在境内）；需要时可在对话框勾选「经本地代理同步」。

## 6. 已知限制

- **TUN 需管理员**：Wintun 建虚拟网卡 + 改路由表，UAC 弹出必须允许。`wintun.dll` 已随包提供。
- **未做代码签名**：首次运行会触发 SmartScreen 提示。
- **CI 未同步**：`.github/workflows/build-nekoray-cmake.yml` 仍是 MSVC + 自定义 Qt 的旧流程，
  与本地 MinGW 流程不一致；发版前建议改成 MinGW 矩阵。
- 仅构建 `amd64`，未做 `arm64`。

## 7. 排错

| 现象 | 原因 | 处理 |
|---|---|---|
| `start core error occurred: 进程启动失败：系统找不到指定的文件` | 内核路径没带 `.exe`（QProcess 在 Windows 上用 `lpApplicationName` 启动，不会自动补后缀） | 已修：`FindNekoBoxCoreRealPath()` 在 `Q_OS_WIN` 下补 `.exe`，`mainwindow.cpp` 改用它而不再硬编码 |
| `This application failed to start because no Qt platform plugin could be initialized` | 直接双击了 **zip 里的** exe，或只解压了 `rekobox.exe` —— `platforms/qwindows.dll` 与 Qt DLL 不在旁边 | 必须**完整解压**整个 `windows64` 目录后，运行目录内的 `rekobox.exe` |
| 启动后立刻退出、无窗口 | 上一次实例残留（单实例锁 `%LOCALAPPDATA%\Rekobox\.instance.lock`） | 任务管理器结束 `rekobox.exe` 后重试 |

自查（部署目录应至少包含）：

```
rekobox.exe  rekobox_core.exe  Qt6Core.dll  Qt6Gui.dll  Qt6Widgets.dll
libgcc_s_seh-1.dll  libstdc++-6.dll  libwinpthread-1.dll
platforms/qwindows.dll  styles/  tls/  imageformats/  wintun.dll
```
