# 批次二修复记录（对齐核心体验）

> 执行时间：2026-11-20 23:30–00:10 · 机器：MacBook Air M2 · macOS 15.7.5 · arm64
> 依据：`RekoBox-mac-排查报告.md` 第五节「批次 2」的 5 项
> 上一批：`BATCH1-FIXES.md`（止血 5 项，已全部完成）
> 结论：**5 项全部完成。其中 1 项（快捷键）经实测推翻了报告判断，改为修真正存在的问题；其余 4 项按报告方向实现并真机验证**

---

## 1. 完成清单

| # | 报告条目 | 状态 | 一句话结论 |
|---|---|---|---|
| 1 | P1-10 快捷键 `Ctrl` → `⌘` | ⚠️ **报告判断有误** | Qt 在 mac 上**已经**把 ⌘ 映射成 `Qt::CTRL`，不需要改 `.ui`；真 bug 是热键注册失败被静默丢弃 |
| 2 | P1-6 托盘与菜单栏共用同一个 `QMenu` | ✅ | mac 上托盘改独立 `QMenu`，只共享 `QAction` |
| 3 | P2-12 托盘模式 Dock 图标仍在 | ✅ | `setActivationPolicy(Accessory)`，实测 `ApplicationType` 从 `Foreground` → `UIElement` |
| 4 | P2-13 唤起窗口不跨应用置前 | ✅ | 加 `activateIgnoringOtherApps`，且修正了调用**顺序** |
| 5 | P2-15 深色模式下仍是浅色界面 | ✅ | `System` 主题改为读 `effectiveAppearance` + 订阅外观变化 |
| 6 | P2-17 无应用日志落盘 | ✅ | `qInstallMessageHandler` → `~/Library/Logs/Rekobox/app.log` |

> 说明：报告批次 2 原文列 5 条（第 6–10 项），其中"托盘菜单独立 QMenu"与"`-tray` 时 setActivationPolicy"
> 在报告里是合并成一条写的，这里按改动拆成两项分别验证。日志（第 10 项）单列。

---

## 2. P1-10 快捷键：报告结论是错的，真问题在别处

### 2.1 实测证据

报告说"`.ui` 里写死 `Ctrl+S`，Qt 不会自动映射 ⌘"。实测不成立：

```
input Ctrl+S   | PortableText: Ctrl+S | NativeText: ⌘S | ctrl: true meta: false
input Ctrl+Alt+T | PortableText: Ctrl+Alt+T | NativeText: ⌥⌘T | ...
input Meta+S   | PortableText: Meta+S | NativeText: ⌃S | ctrl: false meta: true
```

`QKeySequence("Ctrl+S").toString(NativeText)` 就是 `⌘S`。原因是 **Qt 在 macOS 上把
物理 Command 键上报为 `Qt::ControlModifier`，物理 Control 键上报为 `Qt::MetaModifier`**
（与 X11/Windows 的语义互换）。所以：

- `"Ctrl+S"` → 用户按 **⌘S** ✓（本来就是对的行为，菜单里显示的也是 ⌘S）
- 若按报告建议改成 `"Meta+S"` → 用户要按**物理 Control** 键 ✗，等于**制造**一个新 bug

**这一项如果照着报告改，会把好的改成坏的。** 所以 `.ui` 一个字没动。

### 2.2 真正的问题：注册失败被静默丢弃

`ui/mainwindow.cpp` 的 `RegisterHotkey()` 有两处会"静默失败"：

```cpp
for (const auto &key: regstr) {
    if (regstr.count(key) > 1) return;   // ← 重复即 return，后面所有热键都不注册
}
...
} else {
    hk->deleteLater();                   // ← 被别的 App 占用，静默丢弃，界面无任何提示
}
```

mac 上全局热键与系统快捷键（⌘Space、⌘Tab 等）撞车很常见，用户表现就是
"设了没反应，也没有任何解释"。

### 2.3 改了什么

- 新增 `MainWindow::hotkey_failures`（`QStringList`）与 `HotkeyFailureSummary()`；
- `RegisterHotkey()` 不再 `return`，遇到重复/解析失败/注册失败都**记一条并继续**处理后面的；
- 失败信息用 `NativeText` 显示（mac 上写成 `⌘S`，否则提示用户去按的键位本身就是错的）；
- `DialogHotkey` 的保存与重新注册从**析构函数**移到 `finished` 信号：
  原来在析构里做，而那时对象已在销毁、`delete ui` 也在同一路径上；
  现在注册完成后才有结果，正好在 `finished` 之后弹提示，也不会跑在析构的嵌套循环里；
- 有失败项时弹一次说明，指出"多半被别的 App 或系统快捷键占用"，并给出两条处理路径。

---

## 3. P1-6 托盘菜单独立 QMenu

**根因**：mac 走**原生**菜单栏，`QMenuBar` 会变成 `NSMenu`，Qt 把挂在菜单栏上的
`QMenu` 的 platform menu 绑给 `NSMenu`。此时同一个 `QMenu` 再作为托盘右键菜单
`popup()`，两者会互相破坏（托盘空白 / 菜单栏该项消失 / 工具栏下拉弹不出）。

**改法**（`ui/mainwindow.cpp`、`ui/mainwindow.h`）：

- mac 上新建 `tray_menu`（独立 `QMenu`），`tray->setContextMenu(tray_menu)`；
  非 mac 保持原来的 `ui->menu_program` 不变；
- 托盘子菜单（系统代理、当前服务器）也是**新建**的 `QMenu`——
  把 `ui->menu_spmode` 直接塞进来会重新引入同一个 bug；
- `QAction` 可以共享（同一 action 挂多个菜单是合法的，信号只连一次），所以逻辑零重复；
- 内容按 mac 习惯精简：显示窗口 / 系统代理 / 当前服务器（前 20 个）/ 重启代理 / 退出；
- 托盘"当前服务器"超 20 个时给一个 "More…" 直接开主窗口，
  **不能** `setMenu(ui->menuActive_Server)`——那又是把菜单栏的 QMenu 借给托盘，同一个坑；
- 新增一条自检日志（右键托盘时打）：`top-level items: N | targets a separate QMenu = true |
  menu_program still parented to menubar = true`。
  因为终端没有屏幕录制权限、截图看不到菜单栏，这条日志是唯一可验证的凭证。

---

## 4. P2-12 托盘模式隐藏 Dock 图标

**改法**：新增 `sys/macos/Gui.hpp` / `Gui.mm`（ObjC++ 桥接，与 `AutoRunMac.mm` 同风格）：

| 接口 | 作用 |
|---|---|
| `SetDockIconVisible(bool)` | `NSApp.setActivationPolicy(Regular / Accessory)` |
| `ActivateAppIgnoringOtherApps()` | `[NSApp activateIgnoringOtherApps:YES]` |
| `IsSystemDarkMode()` | 读 `NSApp.effectiveAppearance` 的 `bestMatch` |
| `SetAppearanceChangeCallback()` | 订阅 `AppleInterfaceThemeChangedNotification` |

- `main.cpp` / `MainWindow` 构造末尾：`flag_tray` 为真时调 `SetDockIconVisible(false)`；
- `showEvent` 里切回 `Regular`，`closeEvent`（隐藏到托盘）里切回 `Accessory`——
  这样"从托盘唤起 → 有 Dock 图标 → 再关掉 → 又只剩菜单栏"是自洽的。

**为什么不用 `Info.plist` 的 `LSUIElement`**：那是编译期固定值，设了 `true` 就永远
没有 Dock 图标，窗口模式也没法恢复。必须运行时切。

**实测**：

```
$ lsappinfo info -only ApplicationType <pid>
-tray 启动   → "ApplicationType"="UIElement"      ← 无 Dock、不进 Cmd+Tab
正常窗口启动 → "ApplicationType"="Foreground"     ← 与改动前一致
```

---

## 5. P2-13 唤起窗口跨应用置前

`main/NekoGui_Utils.cpp` 的 `ActivateWindow()` 原来 mac 分支只有 `raise()` +
`activateWindow()`，App 不在前台时系统会拒绝抢占，表现是"只闪一下"。

改动重点不只是"加一行"，而是**顺序**：

```cpp
#ifdef Q_OS_MACOS
    NekoGui_mac::ActivateAppIgnoringOtherApps();  // 先让 App 自己抢前台
#endif
    w->raise();                                   // 再抬窗口，否则抬了也会被压回去
    w->activateWindow();
```

先 `raise()` 再 activate 是无效的——窗口层级操作发生在 App 未激活时会被忽略。

---

## 6. P2-15 深色模式：`System` 主题真的跟随系统

**根因不只是"没有深色 QSS"**：`ThemeManager::ApplyTheme("0")` 用的是
`QStyleFactory::create(...)->standardPalette()`，那是 Fusion 的**浅色**默认调色板，
与 `NSApp.effectiveAppearance` 毫无关系。所以就算系统是深色，"跟随系统"也给浅色。

**改法**（`ui/ThemeManager.cpp/.hpp`）：

1. `ApplyTheme(0)` 时先问 `NekoGui_mac::IsSystemDarkMode()`，深色则用 `MacDarkPalette()`
   （中性灰 `#1E1E1E` 底而不是纯黑：纯黑 + 13pt 中文正文对比过强，也更像 macOS）；
   浅色仍用 `standardPalette()`，与原来一致；
2. 新增 `ReapplyTheme()`：清掉 `current_theme` 缓存再重算（否则同参数会被 `return` 跳过）；
3. 新增 `WatchSystemAppearance()`：订阅系统外观变化，**只在当前是 `System` 主题时**重算
   （固定主题 flatgray/lightblue 与系统无关，不该跟着变）；
4. `main.cpp` 在 `QApplication` 之后、建窗口之前挂上订阅。

界面本身没有硬编码颜色（`ui/*.ui` 里 `styleSheet` 数为 0），全部由调色板驱动，
所以换调色板即可覆盖全界面，不需要维护一份深色 QSS。

**实测**（强制 App 级 `DarkAqua` 外观，等价于系统深色下的读取路径）：

```
light (aqua)      | systemDark: false | Window: #ececec | Text: #000000
forced dark aqua  | systemDark: true  | Window: #323232 | Text: #ffffff
appearance observer installed: true
back to aqua      | systemDark: false | Window: #ececec | Text: #000000
```

判断、切换、通知订阅三条路径都通过。

> 局限（如实记录）：**没能在本次会话里用"系统设置里真的切深色"来验证回调触发**。
> macOS 15 的 `System Events → appearance preferences` 需要辅助功能授权（-10004 被拒），
> `defaults write -g AppleInterfaceStyle Dark` 也不会即时改变已运行进程的
> `effectiveAppearance`。上面用 App 级强制外观验证了读取与切换逻辑，
> 通知订阅确认安装成功，但"系统切外观 → 回调真的被调用"这一环需要人工点一次确认。

---

## 7. P2-17 应用日志落盘

新增 `main/AppLog.hpp` / `AppLog.cpp`（三平台通用，非 mac 分支也在用）：

| | 路径 |
|---|---|
| macOS | `~/Library/Logs/Rekobox/app.log`（「控制台」App 侧栏可见） |
| Windows | `<数据目录>/logs/app.log` |
| Linux | `$XDG_STATE_HOME/Rekobox/app.log`（缺省 `~/.local/state`） |

细节：

- 保留原 handler（`g_prev_handler`），所以**从终端跑时 stderr 行为完全不变**——已实测；
- 环形缓冲最近 200 行常驻内存；`QtFatalMsg` 时先写 `crash-<时间戳>.log`
  （含最近日志 + `backtrace_symbols`）再落最后一行；
- 超过 8 MB 轮转成 `app.log.1`，避免长期运行写满磁盘；
- 打不开就静默退回 stderr，**不因为日志把 App 拖死**；
- 启动第一行写版本 / 系统 / 架构，随后 `qInfo() << "[app] log file:" << 路径`——
  让用户报问题时自己就能找到路径。

**实测**：

```
---- RekoBox 1.0.15 starting · macOS 15.7 · arm64 ----
[..] [DEBUG] [mac] tray available: true visible: true supportsMessages: true
[..] [INFO] [app] log file: "/Users/mrcheng/Library/Logs/Rekobox/app.log"
[..] [DEBUG] [sync] pull start: "startup"
[..] [DEBUG] [sync] done: "已是最新（5 个节点）"
...
[..] [DEBUG] signal received, shutting down from the event loop
[..] [DEBUG] Start of data save
[..] [DEBUG] End of data save
```

以前这些 `[mac] …` / `[sync] …` 从 Finder 启动时**全部丢失**，现在都在。

---

## 8. 验证记录

| 项 | 结果 |
|---|---|
| 编译 | ✅ `ninja` 通过；新增 `Gui.mm` 编译单元正常 |
| 链接 | ✅ `otool -L` 含 `AppKit` / `ServiceManagement` / `CoreGraphics` / `Carbon` |
| 签名 | ✅ `codesign --verify --deep --strict` 静默通过 |
| 启动（托盘） | ✅ `ApplicationType=UIElement`（Dock 图标确实消失） |
| 启动（窗口） | ✅ `ApplicationType=Foreground`（与改动前一致，无回归） |
| 日志 | ✅ 落盘 + stderr 双通道；退出链路完整记录 |
| 退出 | ✅ SIGTERM 后 <1s 干净退出 |
| 崩溃报告 | ✅ 仍是 11 份历史文件，**无新增** |
| 深色判定 | ✅ 强制 `DarkAqua` 下 `systemDark=true`，调色板同步变深 |
| Windows 分支 | ✅ 所有 `sys/macos/*` 引用都在 `#ifdef Q_OS_MACOS` 内 |

未能在本会话验证的：

- **托盘菜单实际长相**：终端没有"屏幕录制"权限，`screencapture` 只截到壁纸
  （这正是报告 P1-7 的成因）。已加自检日志，右键托盘一次即可在 `app.log` 里确认结构。
- **系统深色切换的真实回调**：见 §6 局限。
- **跨应用置前的真机手感**：需要"App 在后台 → 从别的 App 点它"这类操作，
  自动化会话做不了。代码路径已改，顺序已按 Apple 的要求调整。

---

## 9. 改动文件清单

| 文件 | 性质 |
|---|---|
| `ui/mainwindow.cpp` | 托盘独立 `QMenu` + 自检日志；`flag_tray` 时隐藏 Dock；`showEvent`/`closeEvent` 切换策略；热键失败收集 |
| `ui/mainwindow.h` | 新增 `tray_menu`、`hotkey_failures`、`HotkeyFailureSummary()`、`showEvent` |
| `ui/dialog_hotkey.cpp` | 保存/注册移到 `finished`；失败提示 |
| `ui/ThemeManager.cpp` | `System` 主题跟随系统深色；`ReapplyTheme()`；`WatchSystemAppearance()` |
| `ui/ThemeManager.hpp` | 新增上述两个方法与 `requested_theme`；补 `#include <QString>` |
| `main/NekoGui_Utils.cpp` | `ActivateWindow` 加 `activateIgnoringOtherApps`（注意顺序） |
| `main/main.cpp` | 安装日志；挂外观订阅 |
| `main/AppLog.cpp` / `AppLog.hpp` | **新增** 日志落盘 + 崩溃报告 |
| `sys/macos/Gui.mm` / `Gui.hpp` | **新增** AppKit 桥接（Dock 策略、置前、深色、外观通知） |
| `cmake/macos/macos.cmake` | 加入 `Gui.mm` 及 `OBJCXX` 属性 |
| `CMakeLists.txt` | 加入 `main/AppLog.cpp` |

---

## 10. 留给批次三的事

按报告第五节，批次三（架构级）尚未动：

- **P0-2 QThread 崩溃群**（最高优先级，仍未定位）：需要 dSYM + lldb。
  本批的日志落盘为它**提供了新的抓手**——`crash-*.log` 里现在有崩溃前的 200 行日志
  和 backtrace，下次复现能直接看"崩溃前在跑什么"。
  但仍建议出带 `-g` 的构建，用 `lldb` 看 faulting thread 的 `QThreadData` 归属。
- P1-8 TUN 下 DNS 真接管（`127.0.0.1:53` + 固定上游）——代码与 HANDOFF 文档仍矛盾
- P1-9 `process_name` 在 darwin 空转（建议禁用输入框 + 改模板里的 `rekobox_core` 名字）
- P1-11 日志面板级别选择器（三平台共用）
- P3-18/19 签名与公证、mac 专属更新通道（Sparkle 2）
- P2-14 文件对话框放开 `AA_DontUseNativeDialogs`
- P2-16 App Nap / 睡眠唤醒 / 换网监听