# 一键检查更新（Android / Windows）

两端共用一套发布约定与版本比对规则，UI 与安装动作各自平台实现。

## 1. 版本检测

### 1.1 数据源

GitHub Releases 列表接口：

```
GET https://api.github.com/repos/tmclsamxy/RekoBox/releases?per_page=30
Accept: application/vnd.github+json
User-Agent: RekoBox/<当前版本>
```

**不要用 `/releases/latest`。** 仓库同时承载两条发布线：

| 平台 | tag 示例 | 资产名 |
|---|---|---|
| Android | `v1.0.1` | `RekoBox-1.0.1-arm64-v8a.apk` |
| Windows | `win-v1.0.4` | `Rekobox-1.0.4-windows-x64-qt.zip` |

`/releases/latest` 返回的是**全仓库**最新（当前会被 Windows 版抢走），Android 端拿到就会下载到 zip。
所以两端都拉列表、按 tag 前缀过滤：

- Android：`tag[0] == 'v'` 且第二字符是数字
- Windows：`tag` 以 `win-v` 开头

Android 还有一条预览通道（`includePreview`），接受 `prerelease: true` 的 `v*` tag 或 `pre-1.0.2-1` 形式。

### 1.2 版本比对

忽略前缀，按 `.` 分段转整数逐段比较，缺位补 0：

```
compareVersion("1.0.10", "1.0.9") = 1     // 不是字符串比较
compareVersion("1.0", "1.0.0") = 0
```

只有 `远端 > 本地` 才提示，不支持降级（避免把预发布误判为新版）。本地版本：

- Android：`BuildConfig.VERSION_NAME`（由 `nb4a.properties` 的 `VERSION_NAME` 注入）
- Windows：`NKR_VERSION`（编译期宏，源自 `nekoray_version.txt`）

### 1.3 资产选择

- Android：按 `Build.SUPPORTED_ABIS` 顺序找 `-<abi>.apk`，找不到退回第一个 `.apk`
- Windows：资产名包含 `windows-x64-qt.zip`

## 2. 触发时机

| 场景 | Android | Windows |
|---|---|---|
| 启动 | `MainActivity.onCreate` → `UpdateLauncher.checkOnStart`，**24 小时节流** | 主窗口构造后 `QTimer::singleShot(8000)` |
| 手动 | 关于页「检查正式版 / 预览版」 | 菜单 `Program → Check for updates` |

启动检查是**静默**的：失败、无更新都不打扰；只有存在新版本才弹窗。
手动检查则会明确反馈「已是最新」或错误原因。

节流的目的：GitHub 匿名 API 限 60 次/小时/IP，每次冷启动都请求既浪费也容易撞限。

## 3. Android：下载 → 拉起安装器

```
点「是」→ 检查「安装未知应用」权限 → OkHttp 流式下载（64 KB buffer）
        → 进度条（已下载/总量 + 百分比）→ FileProvider 授权 → 系统安装器
```

关键实现：

| 环节 | 位置 / 做法 |
|---|---|
| 下载 | `UpdateDownloader`，OkHttp，写 `cacheDir/update/<asset>`，按字节回调进度 |
| 进度 UI | 不可取消的 `MaterialAlertDialog` 对话框，实时显示 `1.2 MB / 16.4 MB  7%` |
| 完整性 | 比对 `Content-Length`，不一致直接删除文件并报错 |
| 安装 | `ApkInstaller.install()`：`FileProvider.getUriForFile(ctx, "io.rekobox.cache", apk)` + `ACTION_VIEW` + `application/vnd.android.package-archive` + `FLAG_GRANT_READ_URI_PERMISSION` |

**为什么不是静默安装**：`INSTALL_PACKAGES` 是系统签名权限，普通应用拿不到。
Android 的「一键」终点就是拉起系统安装器，由用户点一次「安装」——
这一步是平台强制的，任何应用都绕不过（除 Root / 系统签名）。

权限：

```xml
<uses-permission android:name="android.permission.REQUEST_INSTALL_PACKAGES" />
```

Android 8.0+ 该权限是**按应用**授权且默认关闭，所以下载前先查
`packageManager.canRequestPackageInstalls()`，为 false 时弹窗引导到
`Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES`，用户授权后重新检查即可。

`FileProvider` 复用已有配置：authority `${applicationId}.cache`，路径 `cache-path "/"`。

## 4. Windows：下载 → 解压 → 替换 → 重启

```
点「是」→ 下载 zip 到 %TEMP%\rekobox-update\update.zip（进度对话框）
        → Expand-Archive 解到 payload\
        → 定位含 rekobox.exe 的目录（包内是 windows64\）
        → 校验安装目录可写
        → 生成 apply-update.ps1 → startDetached → 主程序退出
        → 脚本等 rekobox.exe 退出 → Copy-Item 覆盖 → 启动新 exe → 自删
```

关键实现（`ui/update/UpdateHelper.cpp`）：

| 环节 | 做法 |
|---|---|
| 下载 | `QNetworkReply::downloadProgress` 驱动 `QProgressDialog` |
| 解压 | 写 `extract.ps1`，`-File` 传参调 `Expand-Archive -LiteralPath` |
| 替换 | 独立 PowerShell 进程；`Get-Process -Name rekobox` 轮询等待主进程退出（最多 120 s） |
| 重启 | `Start-Process -FilePath <exe>`，然后脚本 `Remove-Item $PSCommandPath` 自删 |
| 失败处理 | `try/catch` 写 `%TEMP%\rekobox-update.log`，`finally` 里无论如何重启一个 exe，避免程序彻底消失 |

**为什么必须外部进程**：Windows 不允许覆盖正在运行的 exe（文件被独占），
所以只能退出后由另一个进程完成复制。

**为什么不打安装包**：发行物是绿色 zip，替换就是覆盖同目录文件；新版本只增不删文件，
用 `Copy-Item -Recurse -Force` 覆盖即可，不会误删用户目录里的其它东西。

编码处理：`.ps1` 以 UTF-8 BOM 写入，路径通过 `-File` 参数传递（而不是拼进命令字符串），
`D:\我的 程序\RekoBox` 这类含中文/空格的路径可用 —— 已实测。

权限：

- 普通用户目录（解压到桌面/D 盘）→ 无需管理员
- 安装到 `C:\Program Files` → 覆盖需要管理员，代码先检查
  `QFileInfo(dstDir).isWritable()`，不可写时直接提示「目录不可写，请移动或管理员运行」

## 5. 代理

两端都遵循「用用户当前选中的节点」：

- Android：`UpdateChecker` 走 libcore HTTP 客户端 + `trySocks5(DataStore.mixedPort)`；
  `UpdateDownloader` 用 OkHttp 的 SOCKS 代理指向同一端口，带 `Proxy-Authorization`。
  未连接时端口不监听，第一次请求失败后**自动回退直连**重试一次。
- Windows：`QNetworkProxy::Socks5Proxy` 指向 `dataStore->inbound_socks_port`（mixed 入站），
  需要认证时带用户名密码；请求失败同样回退 `NoProxy` 重试。

这样已连接时更新流量也走代理，未连接时不会因为代理不可用而卡死。

## 6. 兼容性与已知限制

| 项 | 说明 |
|---|---|
| Windows 解压依赖 | `Expand-Archive` 需要 PowerShell 5.0+（Win10/11 自带） |
| 杀软误报 | 更新流程会创建并执行 `.ps1`，个别杀软可能拦截；可改用签名安装包规避 |
| SmartScreen | 首次运行未签名 exe 仍会提示，与更新功能无关 |
| Android 覆盖安装 | 必须同签名；换签名需先卸载（本项目 `release.keystore` 固定） |
| Android 缓存清理 | 下载的 apk 放在 `cacheDir/update`，安装成功后系统可回收；下次下载前会清空该目录 |
| GitHub 限流 | 未认证 60 次/小时/IP，靠 24 小时节流规避；高频用户可加 token |
| 无更新时的引导 | Windows 手动检查会提示「已是最新（1.0.4）」；Android 弹 Toast |

## 7. 发布检查清单

发版时两件事必须对：

1. tag 前缀正确（Android `v*`、Windows `win-v*`），否则另一端会漏检
2. 资产名包含平台标识（`-arm64-v8a.apk` / `windows-x64-qt.zip`），否则客户端选不到包
