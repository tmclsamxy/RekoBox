# RekoboxForWindowsQt

基于 [MatsuriDayo/nekoray](https://github.com/MatsuriDayo/nekoray) `4.0.1-2024-12-12` 的 Windows Qt 客户端，
已改名为 **RekoBox** 并在 Windows 上完成原生构建与运行验证。

> 与同仓库的 `RekoboxForWindows/`（Compose Desktop 重写版）是两条并行路线：
> 这一条保留 nekoray 的 Qt Widgets 界面与 gRPC 架构，改动集中在工具链、构建系统与依赖。

---

## 1. 现状

| 项 | 值 |
|---|---|
| 界面 | Qt 6.7.2 Widgets（MinGW 13.1 构建） |
| 内核 | `rekobox_core.exe` = sing-box `1.9.7-neko-1` + libneko |
| 编译器 | MinGW-w64 GCC 13.1（原工程 CI 用 MSVC 14.2） |
| 构建 | CMake 3.31 + Ninja |
| 产物 | `deployment/windows64/`（95 MB），打包 `dist/Rekobox-1.0.0-windows-x64-qt.zip`（34 MB） |
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
cd RekoboxForWindowsQt
bash libs/build_all_mingw.sh        # 首次约 15 分钟
```

产物：`deployment/windows64/rekobox.exe`，直接双击运行，无需另装 Qt。

---

## 4. 为什么核心仍停在 sing-box 1.9.7-neko

fork `MatsuriDayo/sing-box` 的分支情况：

| 分支 | 版本 | 是否含 nekoray 所需的 `neko_run.go` 补丁 |
|---|---|---|
| `1.9.7`（当前使用） | 1.9.7-neko-1 | ✅ `cmd/sing-box` 是 `package boxmain`，导出 `Create()` / `SetDisableColor()` |
| `1.11.x` / `1.12.x` / `1.14.x` | 1.11–1.14 | ❌ 恢复成 `package main`，补丁被移除 |

即**上游在新分支里删掉了 NekoBox 专用的可导入入口**，不能简单改 commit。

要升到 1.12/1.14，需要三件事（本轮未做，避免破坏可构建状态）：

1. 在新分支恢复可导入入口（`package boxmain`，或在 `cmd/sing-box` 下加 `internal/neko` 包），
   重写 `Create(config []byte)`：`option.Options.UnmarshalJSON` → `UnmarshalJSONContext`、
   `box.New` 需显式传 `Context` 与 inbound/outbound 注册表、`Start()` 签名变化。
2. 适配 `grpc_box.go`：`Router().SetV2RayServer` 在 1.11+ 已移除，统计改走 `experimental.v2ray_api`；
   `instance.SetLogWritter` 换成新日志接口。
3. `boxapi.DialUDP` 在新分支已不存在，`neko_common.DialUDP` 需改用 `sing-tun` 接口。

推进方式：把 `_deps/sing-box` 换成目标分支 → `go build` 数错误 → 逐个收敛。
建议 1.11 → 1.12 → 1.14 三跳，每跳跑一次实机连通性测试。

---

## 5. 已知限制

- **TUN 需管理员**：Wintun 建虚拟网卡 + 改路由表，UAC 弹出必须允许。`wintun.dll` 已随包提供。
- **未做代码签名**：首次运行会触发 SmartScreen 提示。
- **CI 未同步**：`.github/workflows/build-nekoray-cmake.yml` 仍是 MSVC + 自定义 Qt 的旧流程，
  与本地 MinGW 流程不一致；发版前建议改成 MinGW 矩阵。
- 仅构建 `amd64`，未做 `arm64`。

## 6. 排错

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
