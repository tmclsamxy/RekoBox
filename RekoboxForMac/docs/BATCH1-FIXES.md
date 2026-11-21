# 批次一修复记录（止血 · 不碰架构）

> 执行时间：2026-11-20 22:30–23:20 · 机器：MacBook Air M2 · macOS 15.7.5 · arm64
> 依据：`RekoBox-mac-排查报告.md` 第五节「批次 1」的 5 项
> 结论：**5 项全部完成，编译通过（含 1 个新增 ObjC++ 编译单元），真机启动 / SIGTERM 退出干净，无新增崩溃报告**

---

## 1. 完成清单

| # | 报告条目 | 状态 | 关键改动 |
|---|---|---|---|
| 1 | P0-1 / P0-3 停 TUN 的 `waitForFinished` 无超时 | ✅ | 拆出**同步**与**异步**两条路径 |
| 2 | P0-1 `mainwindow.cpp:737` 退出链路 `waitForFinished(3000)` | ✅ | 改成只 `kill()`，不再等 |
| 3 | P1-5 开机自启（`-tray` 缺失 + `CFRelease` 泄漏） | ✅ **且发现原方案整体失效** | 换 `SMAppService`，见 §3 |
| 4 | P1-4 自动更新（硬编码 win-v / rekobox.exe） | ✅ | 按平台切 tag + 资产 + dmg 安装路径 |
| 5 | P1-7 二维码「从屏幕导入」恒失败 | ✅ | 先查屏幕录制权限，再给引导 |

---

## 2. 逐项说明

### 2.1 停 TUN：不再阻塞 UI，也不再"谎报已关闭"（P0-1 / P0-3）

**改了什么**（`ui/mainwindow.cpp`）

- 新增 `stop_tun_process_blocking()`：真正去杀内核的纯函数，不碰 UI、不改成员变量，
  可以安全跑在 worker 线程里。三平台都带**20 秒上限**（原来 mac 是无参 = 30 秒无限等）。
- `StopVPNProcess()` 保留同步语义（**只给**退出链路 + 疑难解答「重置」用）。
- 新增 `StopVPNProcessAsync()`：用户主动取消勾选时走这条——worker 线程里等
  `osascript`，UI 线程立刻返回。
- `neko_set_spmode_vpn()` 新增 `sync` 参数：`false`（默认）走异步，`true` 只用于退出链路。

**修掉的真实故障链**

原来用户取消勾选 TUN：

```
弹密码框 → 用户点"取消" → p.exitCode() != 0 → ok = false
→ 弹 "Failed to stop Tun process" → neko_set_spmode_FAILED
→ spmode_vpn 仍是 true，但界面已经按新状态刷过 → 勾选框弹回
→ utun 与 72 条路由仍留在系统里 → 整机断网
```

现在：

- 异步期间保持 `spmode_vpn = true`，**失败就明确告警 + 勾选框回到「已开启」**，
  与实际网络状态一致，不再谎报。
- 新增 `vpn_stopping` 防重入（连点勾选框只会提示"正在停止"）。
- 退出链路特殊处理：**停不掉也继续退出**。原来失败会 `retry` 解锁后再无第二次触发，
  程序永远关不掉——那是比"路由残留"更糟的死局。残留有 `RecoverTunDNSIfNeeded()`
  在下次启动兜底，关不掉没有。

### 2.2 退出链路去掉 `waitForFinished`（P0-1 第 2 项）

`mainwindow.cpp` 退出兜底里 `kill()` 后紧跟的 `waitForFinished(3000)` 已删除。
本函数末尾就是 `QCoreApplication::quit()`，在那里跑嵌套事件循环正是今日 5 份崩溃
报告（`aboutToQuit → waitForFinished → ~QSocketNotifier → SIGSEGV`）的成因；
先 kill 再等，等于把同一处崩溃往前挪了一步。`kill` 是 SIGKILL，端口与文件句柄
内核立刻回收，不需要等。

另外顺手修了 `exit_reason == 1`：mac 上**故意没有** `updater`（`main.cpp:112` 不建软链、
`build_macos.sh:198` 还会 `rm -f`），原来 `startDetached("./updater")` 必然失败且不报错，
用户看到的是"更新完 App 直接消失"。现在 mac 分支只记日志，重启交给 dmg 安装流程。

### 2.3 二维码「从屏幕导入」（P1-7）

`sys/macos/MacPlatform.cpp` 新增 `HasScreenCaptureAccess()` / `RequestScreenCaptureAccess()`
（`CGPreflightScreenCaptureAccess` / `CGRequestScreenCaptureAccess`）。
`on_menu_scan_qr_triggered()` 先查权限，没权限就弹说明框 + 一键打开
「系统设置 → 隐私与安全性 → 屏幕录制」，并提示**授权后必须重启 App**。

> 为什么这不是"多余的一步"：没有权限时 `grabWindow` **不报错**，只返回一张把窗口
> 内容整块剔除的图（只剩壁纸），所以原来看起来像"二维码识别坏了"。

识别失败时的提示也补了一句权限相关的话，避免用户反复重试。

### 2.4 自动更新（P1-4）

`ui/update/UpdateHelper.cpp` 原来是三重硬编码：`win-v` + `windows-x64-qt.zip` + `rekobox.exe`。

现在按平台切：

| | tag 前缀 | 资产关键字 | 扩展名 |
|---|---|---|---|
| macOS | `mac-v` | `mac-arm64` | `.dmg` |
| Windows | `win-v` | `windows-x64-qt` | `.zip` |
| Linux | 无发布线 | — | — |

mac 的安装路径改为 **`open <dmg>` + 让用户拖进 Applications**，不再自己解压替换
（正在运行的 bundle 被原地替换会被 Gatekeeper 判「已损坏」）。

**端到端验证**（本地起假的 release 列表，同时挂 `win-v1.0.99` 与 `mac-v1.0.16`）：

```
[update] picked tag "mac-v1.0.16" asset "Rekobox-1.0.16-mac-arm64.dmg" platform filter "mac-v" "mac-arm64"
```

选对了 mac 包，没再拉 Windows 包。这两行日志留在代码里，以后出问题不用靠猜。

### 2.5 开机自启（P1-5）—— 原方案已整体失效

**这一项和报告的判断不一样，实测推翻了报告里的推测。**

报告推测是「`LSSharedFileListInsertItemURL` 写入成功但系统不认」。实际情况更糟：

```
# 本机写的探针程序（macOS 15.7.5 / arm64）
target: .../RekoBox.app
  snapshot total=3
before: 0
insert -> item NULL          ← 直接返回 NULL
after insert: 0              ← 列表里从来没有我们这一项
after remove: 0
```

**`LSSharedFileList` 在这套系统上已经彻底不能用**，不是"写进去读不回来"。
所以只在原实现上修 `CFRelease` 泄漏，功能依然是坏的——那 3 个内存 bug（命中即
`return true` 泄漏 4 个 CF 对象、提前 return 漏 release、对可能为 NULL 的
`loginItems` 调 `CFRelease`）确实是真的，但修好也不解决问题。

**改用 `SMAppService.mainAppService`**（macOS 13+，正是部署目标）：

- 新增 `sys/macos/AutoRunMac.hpp` / `AutoRunMac.mm`（ObjC++ 桥接层）
- `cmake/macos/macos.cmake` 开 `enable_language(OBJCXX)` + 链接 `ServiceManagement.framework`
- `sys/AutoRun.cpp` 的 mac 分支整体替换（`LSSharedFileList` 相关代码全删）

**关于 `-tray`**：`mainAppService` 拉起 App 时**无法携带命令行参数**，抄不了 Windows 的
`"…\rekobox.exe" -tray`。改为启用自启时把 `start_minimal` 置 `true`，
`main.cpp:285` 会据此走纯托盘——效果与 Windows 的 `-tray` 等价（不弹主窗口，只留托盘图标）。
只在开启时改，关闭自启不反向覆盖用户的界面偏好。

**顺带修掉"勾了又跳回去"的第二个原因**：`mainAppService` 只认**位于 `/Applications`**
的 App。直接跑 `build/RekoBox.app`（开发调试）时状态恒为 `NotFound`。所以：

- `AutoRun_SetEnabled()` 改签名 `void → bool`（三平台统一），失败时界面**复位勾选框并弹提示**
  告诉用户"要放进 Applications"；
- 新增 `AutoRun_Status()` 三态，把 `RequiresApproval`（已登记等批准）和 `NotRegistered`
  区分开，前者勾选框保持勾上并提示去系统设置批准，不再无故弹回。

---

## 3. 验证记录

| 项 | 结果 |
|---|---|
| 编译 | ✅ `ninja` 全量通过；新增 `.mm` 编译单元正常；无新增 warning |
| 链接 | ✅ `otool -L` 可见 `CoreGraphics.framework` 与 `ServiceManagement.framework` |
| 签名 | ✅ `codesign --verify --deep --strict` 静默通过（改动后重新 ad-hoc 签名） |
| 启动 | ✅ `[mac] tray available: true visible: true`，云端同步正常 |
| 退出 | ✅ SIGTERM 后 4 秒内干净退出 |
| 崩溃报告 | ✅ 11 份仍是 19:52 之前的历史文件，**本次无新增** |
| 更新选择 | ✅ 见 §2.4 的日志 |

未能在本会话验证的：

- **TUN 启停实测**（会弹系统密码框，无法在自动化会话里完成）。代码路径已改但未跑通。
- **`AutoRun_SetEnabled()` 注册实测**：本机 `build/` 里的 .app 不在 `/Applications`，
  实测状态是 `NotFound`（这本身证明了 §2.5 的判断）；真正的注册成功与否需要把 App
  拷到 `/Applications` 后手点一次勾选框。

---

## 4. 改动文件清单

| 文件 | 性质 |
|---|---|
| `ui/mainwindow.cpp` | 退出链路去 `waitForFinished`；停 TUN 拆同步/异步；二维码权限；自启失败处理 |
| `ui/mainwindow.h` | `neko_set_spmode_vpn` 加 `sync`；新增 `StopVPNProcessAsync`；新增 `vpn_stopping` |
| `sys/macos/MacPlatform.hpp` | 新增屏幕录制权限接口 |
| `sys/macos/MacPlatform.cpp` | 实现 `HasScreenCaptureAccess` / `RequestScreenCaptureAccess` |
| `sys/macos/AutoRunMac.hpp` | **新增** SMAppService 接口（三态状态 + 设置） |
| `sys/macos/AutoRunMac.mm` | **新增** SMAppService 桥接（ObjC++） |
| `sys/AutoRun.cpp` | mac 分支整体换 `SMAppService`；三平台 `SetEnabled` 改返回 bool；新增 `Status` |
| `sys/AutoRun.hpp` | 签名与三态语义说明 |
| `ui/update/UpdateHelper.cpp` | 按平台切 tag/资产/安装方式；mac 用 `open dmg`；平台日志 |
| `cmake/macos/macos.cmake` | 开 `OBJCXX`；加 `AutoRunMac.mm`、`ServiceManagement`、`CoreGraphics` |

---

## 5. 留给批次二/三的事

批次一没碰、仍待处理（按报告原顺序）：

- **P0-2 QThread 崩溃群**：仍是最高优先级，需要 dSYM + lldb 定位。
  本轮改动**降低**了触发概率（退出链路不再有嵌套事件循环、TUN 启停从 UI 线程移出），
  但根因未定位——报告第五节把它列在批次 3，建议提前。
- P1-6 托盘与菜单栏复用同一个 `QMenu`
- P1-8 TUN 下 DNS 真接管（代码与 HANDOFF 文档仍互相矛盾）
- P1-9 `process_name` 在 darwin 空转
- P1-10 快捷键 `Ctrl` → `⌘`
- P1-11 日志面板过滤（建议改成三平台共用级别选择器）
- P2-12 ~ P2-17、P3-18/19

另：`AutoRunMac.mm` 引入后，`sys/AutoRun.cpp` 里的
`#include <CoreServices/CoreServices.h>` 已移除（不再需要 `LSSharedFileList`），
若后续有人加回相关代码，记得同时把 `CoreServices` 链接项也加回 `macos.cmake`
（目前仍有保留，无副作用）。