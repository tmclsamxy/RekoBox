#pragma once

#include <QString>

#include <functional>

// macOS 的 AppKit 交互层（Objective-C++）。与 MacPlatform.cpp 分工：
//   MacPlatform.cpp  纯 C++，处理网络/命令行/权限这类能走 C API 的事
//   Gui.mm           必须碰 NSApplication / NSWindow 的事（Dock 图标、置前、深色外观）
//
// 单独开一个 .mm 是因为这些 API 没有 C 等价物，硬塞进 .cpp 会编译不过。
namespace NekoGui_mac {

    // 托盘（菜单栏）模式：让 App 从"普通应用"变成"配件应用"——
    // 不再占 Dock 图标，也不出现在 Cmd+Tab 里，只留菜单栏图标。
    // 对应 Windows 上 -tray 是纯托盘（无任务栏按钮）的语义。
    //
    // 必须成对使用：显示主窗口时切回 Regular，否则窗口会浮不出 Dock 的层级，
    // 且用户按 Cmd+Tab 找不到它。
    void SetDockIconVisible(bool visible);

    // 把窗口提到最前，包括 App 本身不在前台时。
    // Qt 的 raise() + activateWindow() 在 App 不是前台进程时通常只闪一下
    // （Windows 分支额外调了 SetForegroundWindow，mac 之前没有对应实现）。
    void ActivateAppIgnoringOtherApps();

    // 系统当前是不是深色外观（读 NSApp.effectiveAppearance）。
    bool IsSystemDarkMode();

    // 订阅"系统外观变化"（用户在设置里切换深色/浅色、或按时间自动切换）。
    // callback 会在主线程被调用；传空函数即取消订阅。返回是否订阅成功。
    // 回调里不要再调 SetDockIconVisible 之类的窗口层级操作，只适合重新 ApplyTheme。
    bool SetAppearanceChangeCallback(const std::function<void()> &callback);

    // ------------------------------------------------------------------
    // App Nap 抑制 / 睡眠唤醒 / 网络变化（P2-16）
    //
    // 三个都是"没有就会出怪现象"的系统行为，且都只能从 AppKit / 系统通知拿：
    //
    //   App Nap：没抑制时，窗口不可见一段时间后系统会节流定时器。
    //            TrafficLooper 靠 QThread::msleep 轮询，最先中招——
    //            表现是"挂后台久了流量统计不跳了"。
    //            用 NSProcessInfo beginActivityWithOptions 拿一个
    //            UserInitiatedAllowingIdleSystemSleep 的 activity 句柄即可；
    //            注意要**长期持有**，释放了抑制就没了。
    //
    //   睡眠唤醒：utun 与路由在睡眠期间可能被系统拆掉，唤醒后 TUN 看着是"开着"，
    //            实际已经不通。订阅 NSWorkspaceDidWakeNotification，唤醒后让上层
    //            重启 TUN（或至少重建路由）。
    //
    //   网络变化：Wi-Fi 切有线 / 换 SSID 后，auto_detect_interface 会自己挑接口，
    //            但 route_exclude_address 里的节点 IP 可能来自旧 DNS，已经过期。
    //            订阅 SCNetworkReachability 太绕，这里用网络服务配置变化通知
    //            （SystemConfiguration 的 State:/Network/Global/IPv4 变化）。
    //
    // 回调都在主线程投递；返回 false 表示订阅失败（不致命，记日志即可）。
    // ------------------------------------------------------------------

    // 开始/结束 App Nap 抑制。可重复调用，内部按引用计数。
    void SetAppNapInhibited(bool inhibited);

    // wake_cb：系统唤醒后调用（主线程）；net_cb：网络配置变化后调用（主线程）。
    // 传空函数即取消订阅。
    bool SetPowerEventCallbacks(const std::function<void()> &wake_cb,
                                const std::function<void()> &net_cb);

} // namespace NekoGui_mac