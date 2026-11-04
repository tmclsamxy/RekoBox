# Rekobox for Windows — 移植方案与工程骨架

> 目标：基于 `RekoboxForAndroid` 的引擎与协议栈，产出一个可在 Windows 10/11 x64 原生运行、
> 带图形界面、支持 TUN 与系统代理两种模式的代理客户端。
> **注意**：这是一个已被放弃的设计（Compose Desktop 重写版），工程已从仓库移除；
> 现在 Windows 端以 `RekoboxForWindows/`（nekoray + Qt6）为准。本文仅作设计参考保留。

---

## 1. 先说结论：为什么不能"把 Android 工程改吧改吧编译成 exe"

Android 版的架构是**进程内**的：

```
MainActivity ──► ProxyService(:bg) ──► VpnService.establish()  ──► fd
                                          │
                                          ▼
                              libcore.aar (gomobile 绑定, sing-box)
                                          │
                                          ▼
                              hev-socks5-tunnel (tun2socks, C, JNI)
```

三道硬坎：

| 卡点 | 具体问题 |
|---|---|
| `gomobile bind` 只支持 `android` / `ios` | 没有 `windows` target。Windows 要进程内嵌核心只能走 `go build -buildmode=c-shared` 自己写 JNI 桥 |
| 隧道建立方向相反 | Android 由 App 用 `VpnService` 拿 fd 交给核心；Windows 上**没有人能替你创建虚拟网卡**，必须由核心自己用 Wintun 建 |
| UI 栈不可移植 | `ui/` 49 个 Fragment + `widget/` 13 个自定义 View + 46 个 layout XML + `preference-kt`，全部是 Android 专有 API |

所以路线是：**引擎降级为独立进程，GUI 只做配置与遥控**。

---

## 2. 架构对照

| 关注点 | Android 版 | Windows 版 |
|---|---|---|
| 内核装载 | `libcore.aar`（gomobile），同进程 | `sing-box.exe` 子进程，`run -c config.json` |
| 隧道 | `VpnService` fd → hev tun2socks | 核心自建 `tun` 入站（Wintun + gvisor stack） |
| 控制面 | AIDL `ISagerNetService` | Clash API `http://127.0.0.1:9090` |
| 流量统计 | AIDL 回调推送 | `GET /traffic`（逐行 JSON 流） |
| 日志 | AIDL + 文件 | 子进程 stderr + `GET /logs` |
| 分应用代理 | `proxied_apps`（VpnService 支持 UID 黑白名单） | **无对应机制**，改用路由规则 `process_name` |
| 系统代理 | 不存在该概念 | 注册表 `Internet Settings` + `InternetSetOption` 刷新 |
| 权限模型 | `VpnService.prepare()` 授权弹窗 | UAC 管理员（仅 TUN 需要） |
| 开机自启 | `BootReceiver` | `HKCU\…\Run` 或计划任务 |
| 常驻/通知 | 前台 Service + 通知渠道 | 托盘图标 |
| 插件 | 独立 APK + AIDL 协议 | 直接 exec 第三方二进制（无 APK 概念） |

---

## 3. 目录结构（已生成）

```
RekoboxForWindows/
├── settings.gradle.kts
├── build.gradle.kts                  # 插件版本集中声明（Kotlin 2.0.21 / Compose 1.7.3）
├── gradle.properties
├── gradlew / gradlew.bat / gradle/wrapper/
│
├── core/                             # 纯 JVM，无 UI 依赖
│   └── src/main/kotlin/io/rekobox/core/
│       ├── AppPaths.kt               # 数据目录 %LOCALAPPDATA%\RekoBox、内核二进制查找
│       ├── engine/SingBoxEngine.kt   # 子进程生命周期、stderr 泵、状态机
│       ├── clash/ClashApi.kt         # /version /proxies /traffic /logs /connections
│       ├── config/ConfigBuilder.kt   # 生成 sing-box JSON（tun/mixed/dns/route/experimental）
│       └── profile/ProfileParser.kt  # 分享链接 → outbound（首版 ss/socks/http/JSON）
│
├── app/                              # Compose Desktop 界面
│   └── src/main/kotlin/io/rekobox/win/
│       ├── Main.kt                   # 窗口入口
│       ├── ui/App.kt                 # 状态栏 + 导航 + 节点/设置/日志三页
│       └── platform/
│           ├── SystemProxy.kt        # WinINet 系统代理开关
│           └── StartupAndElevation.kt# 提权、开机自启
│
├── runtime/
│   ├── windows/                      # sing-box.exe + wintun.dll（fetch-core.ps1 下载到此）
│   └── common/                       # 跨平台随包资源（图标等）
│
├── scripts/fetch-core.ps1            # 拉取内核与 Wintun
└── README.md
```

`runtime/windows/` 是 Compose Desktop `appResourcesRootDir` 约定的目录名 ——
打包时里面的文件会被复制到安装目录根，`AppPaths.findCoreBinary()` 正是按这个位置找。

---

## 4. 适配改造清单

### 4.1 内核层（新增，Android 侧无对应代码）

| 事项 | 做法 |
|---|---|
| 内核二进制 | 用上游 `SagerNet/sing-box` 官方 Windows 构建；若要 NekoBox 专有协议（shadowtls-anytls 等），从 `libcore` 的 `go.mod` replace 分支自行 `go build` |
| Wintun | `wintun.dll` 必须与 exe 同目录；sing-box 启动时由 `tun` 入站自动加载 |
| 启动参数 | `sing-box run -c <配置>`；`-D` 指定工作目录，日志走 stderr |
| 升级 | 版本号写进 `runtime/windows/VERSION`，GUI 启动时校验并提示 |

**关键取舍**：不把 `libcore` 编成 DLL，因为需要重写
`platform_box.go`（`UseProcFS`、`VpnService` fd、`protect()` 全部 Android 语义）、
`dns_android.go`、`assets_android.go`，工作量远大于收益，而 sing-box 官方 Windows 构建
已经覆盖 99% 的协议。

### 4.2 可直接复用的 Kotlin 代码（从 Android 侧搬）

| Android 包 | 文件数 | 复用方式 |
|---|---|---|
| `fmt/`（各协议解析器 + gson 模型） | 22 | 纯 Kotlin + gson，几乎零改动；只需把 `SagerNet.app` 之类全局单例换成参数注入 |
| `database/`（Room 实体/DAO/migration） | 13 | 换 Room KMP（`androidx.room:room-runtime` + `BundledSQLiteDriver`）或改 SQLDelight |
| `ktx/`（协程、日志、格式化工具） | 11 | 直接搬，剔除 `Context` 扩展 |
| `group/`（策略组选择逻辑） | 3 | 逻辑可留，实现改走 Clash API `/proxies` |
| `nb4a/utils`（部分纯函数） | 少量 | 视依赖逐个搬 |

合计约 **49 个文件可复用**，占 Android 端 Kotlin 代码的 28%。

### 4.3 必须重写

| Android 包 | 文件数 | 原因 |
|---|---|---|
| `ui/` | 49 | Fragment + ViewBinding + Material Components，全部换成 Compose |
| `bg/` | 16 | `VpnService` / `ProxyService` / AIDL binder / 前台通知 |
| `widget/` | 13 | 自定义 View（`StatsBar`、`FabProgressBehavior`…）→ Compose 组件 |
| `plugin/` | 1 | 依赖 Android `PackageManager` 与插件 APK |
| `SagerNet.kt` / `SagerNet` Application | 1 | 进程模型完全不同 |
| `res/layout/*` | 46 个 XML | 全部废弃 |

### 4.4 平台集成层（Windows 专有，Android 侧无对应）

| 能力 | 实现要点 | 骨架中的文件 |
|---|---|---|
| 系统代理 | 写 `HKCU\...\Internet Settings` 的 `ProxyEnable`/`ProxyServer`/`ProxyOverride`，再调 `InternetSetOption(39/37)` 通知 WinINet，否则要重启浏览器 | `platform/SystemProxy.kt` |
| TUN 提权 | `net session` 判断是否管理员；非管理员用 `Start-Process -Verb RunAs` 重启自身 | `platform/StartupAndElevation.kt` |
| 开机自启 | `HKCU\...\Run` 写 `"exe" --minimized` | 同上 |
| 托盘 | Compose Desktop `Tray`（`application { Tray(...) }`），最小化到托盘 | 待补 |
| 防火墙 | 首次运行 TUN 会弹 Windows 防火墙授权，需在文档里提示用户点"允许" | — |
| 退出清理 | `Runtime.addShutdownHook` 里 `stop()` 内核 + 关系统代理，否则代理残留导致断网 | 待补 |

### 4.5 打包

```kotlin
compose.desktop.application.nativeDistributions {
    targetFormats(TargetFormat.Msi, TargetFormat.Exe)
    packageName = "RekoBox"
    windows { upgradeUuid = "…" ; perUserInstall = false }
}
```

`./gradlew :app:packageMsi` 产出安装包；`jpackage` 需要 WiX Toolset 3.x 在 PATH 中。
若不想装 WiX，用 `:app:createDistributable` 出免安装目录。

---

## 5. 实施步骤

### Phase 0 — 环境（0.5 天）

```bash
# JDK 17+、Gradle wrapper 已在仓库里
powershell -ExecutionPolicy Bypass -File scripts/fetch-core.ps1
./gradlew :core:compileKotlin          # 只验编译
./gradlew :app:run                     # 起 GUI
```

### Phase 1 — 打通最小闭环（1–2 天）

1. `scripts/fetch-core.ps1` 下好 `sing-box.exe` + `wintun.dll`
2. 粘贴一个 `ss://` 链接 → 点「导入」→ 点「启动」
3. 验证：`curl --proxy http://127.0.0.1:2080 https://ip.sb` 出口 IP 变化
4. TUN 模式：以管理员运行，`route print` 应看到 `172.19.0.1` 的 tun 网卡

**验收标准**：日志里出现 `sing-box started`，状态灯变青，`/traffic` 有数字跳动。

### Phase 2 — 补齐核心功能（1–2 周）

| 任务 | 说明 |
|---|---|
| 移植 `fmt/` 全套解析器 | 按协议逐个搬，配单测（原 Android 端无测试，正好补上） |
| 订阅拉取 + 定时更新 | 复用 `BackupFragment` 的 WebDAV 逻辑改成本地文件 + HTTP |
| 策略组 UI | 读 `/proxies` 的 `Selector`/`URLTest`，调 `PUT /proxies/{group}` 切换 |
| 连接列表 | `/connections` + `DELETE` 断开 |
| 规则编辑 | 路由规则表 + `process_name` 分应用（替代 Android 的 UID 方案） |
| 托盘 + 单实例 | `Tray` + `FileLock` 防多开 |
| 配置迁移 | 导入 Android 的 `rekobox_backup_*.zip`（格式兼容，见 `BackupFragment.kt`） |

### Phase 3 — 打磨（1 周）

- 深色/浅色主题跟随系统
- 开机自启 + 静默启动
- 崩溃日志落盘（对应 Android 的 `CrashHandler`）
- 自动更新（GitHub Releases API，复用 `Branding.API_RELEASES_LATEST`）
- MSI 签名（可选，未签名会触发 SmartScreen 警告）

### Phase 4 — 发布

```bash
./gradlew :app:packageMsi
# 产物：app/build/compose/binaries/main/msi/RekoBox-1.0.0.msi
```

GPL-3.0 要求：仓库公开 + 发布页附源码 tag 链接。

---

## 6. 已知限制（要写进产品说明，别让用户踩）

| 限制 | 说明 | 缓解 |
|---|---|---|
| 无「分应用代理」 | Windows 没有 VpnService 那种 UID 白名单 | 用 `route.rules[].process_name` 按进程名分流 |
| TUN 必须管理员 | Wintun 建网卡需要特权 | 非管理员自动降级到系统代理模式（骨架已实现） |
| 系统代理只覆盖 WinINet | 走裸 socket 的程序（部分游戏、CLI）不受影响 | 文档说明；或引导用 TUN |
| API 版本漂移 | sing-box 1.11 起 DNS 配置 schema 变过（`servers[].type`） | 锁内核版本（`fetch-core.ps1` 默认 1.10.7），升级时同步改 `ConfigBuilder` |
| 插件不可用 | Android 插件是 APK | 在 Windows 侧改为直接引用二进制路径 |
| 无 C 层加速 | Android 有 hev-socks5-tunnel | sing-box 自带 gvisor/system stack，性能足够 |

---

## 7. 与 Android 工程的边界

两套工程**各自独立构建**，不共享 Gradle 依赖：

| 共享内容 | 方式 |
|---|---|
| 协议解析器 | 手工复制到 `core/`（后续可抽成 KMP 模块两边共用） |
| 备份文件格式 | 保持 JSON 结构一致，可互相导入 |
| 品牌资源 | `Branding` 常量两边同名同值，便于统一改 |

如果后续想彻底消除重复，把 `fmt/` + `database/` 抽成 Kotlin Multiplatform 模块
（`shared/`），Android 与 Windows 各引一份 —— 这是最干净的终局形态，
但需要先解决 Room 的 KMP 化，建议放到 Phase 5。
