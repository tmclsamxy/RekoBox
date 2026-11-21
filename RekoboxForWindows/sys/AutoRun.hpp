#pragma once

// 开机自启（登录项）。Windows 写注册表 / Linux 写 autostart desktop 文件 /
// macOS 用 SMAppService（见 sys/macos/AutoRunMac.mm）。
//
// **返回值语义**：
//   AutoRun_SetEnabled 返回 false = 没设置成功，界面应把勾选框复位并提示原因。
//   macOS 上 SMAppService 的 mainAppService 只认位于 /Applications 的 App，
//   直接跑 build/ 里的 .app 时状态恒为 NotFound（勾了又跳回去就是这个原因）。
//
//   AutoRun_IsEnabled 把"已登记但待用户批准"也算已启用，避免勾选状态无故弹回；
//   要区分这两种状态用 AutoRun_Status()。

bool AutoRun_SetEnabled(bool enable);

bool AutoRun_IsEnabled();

// 三态：0 = 未启用；1 = 已启用；2 = 已登记但需用户在「系统设置 → 通用 → 登录项」批准。
// 仅 macOS 会返回 2；Windows/Linux 只有 0/1。
int AutoRun_Status();