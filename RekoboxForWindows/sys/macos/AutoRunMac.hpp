#pragma once

// macOS 开机自启（登录项）——用 ServiceManagement 的 SMAppService，而不是
// CoreServices 的 LSSharedFileList。
//
// 为什么换：
//   2026-11-20 本机实测（macOS 15.7.5 / arm64）：LSSharedFileListInsertItemURL
//   直接返回 NULL，登录项列表根本写不进去（snapshot 里从来没有我们这一项）。
//   也就是说旧实现不是"写入成功但系统不认"，而是**整个 API 已不可用**——
//   只修 CFRelease 泄漏，功能依然是坏的。
//
// SMAppService.mainAppService 是 macOS 13+ 的官方做法：不需要 root、不需要特权助手，
// App 自己把自己注册进「系统设置 → 通用 → 登录项」。
//
// 限制：mainAppService 启动 app 时**不能带命令行参数**（所以没法像 Windows 那样传
// -tray）。开机静默进托盘改由 start_minimal 表达，见 sys/AutoRun.cpp 的说明。

namespace NekoGui_mac {

    // 与 SMAppServiceStatus 对齐，但用 int 暴露给 C++，避免把 ObjC 头文件传染到别处
    enum AutoRunStatus {
        AutoRun_NotRegistered = 0, // 没注册
        AutoRun_Enabled = 1,       // 已注册且系统已生效
        AutoRun_RequiresApproval = 2, // 已注册，但用户在「系统设置」里关掉了，需要批准
        AutoRun_NotFound = 3,      // 系统找不到这个 app（多数是没放进 /Applications）
        AutoRun_Unsupported = 4,   // 系统版本低于 13.0
    };

    int AutoRun_Status();

    // true 表示"现在确实是开机自启的"（Enabled 或 RequiresApproval 都算用户已表达意愿，
    // 但 RequiresApproval 时调用方应当提示去系统设置打开）
    bool AutoRun_IsEnabled();

    // 返回 true 表示注册/注销调用成功（不代表系统立刻生效，用 Status() 复核）
    bool AutoRun_SetEnabled(bool enable);

} // namespace NekoGui_mac