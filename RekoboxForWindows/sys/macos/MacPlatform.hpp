#pragma once

#include <QString>

// macOS 平台层：只放"别处不适合放"的东西。
// 命名空间刻意与 NekoGui 分开，避免和 Windows/Linux 的同名符号打架。
namespace NekoGui_mac {

    // 数据目录：~/Library/Application Support/rekobox
    // main.cpp 的 appdata 分支用它兜底（AppConfigLocation 在 mac 上是 Preferences，不能放数据）
    QString DataDir();

    // 一次性外观设置：固定 Fusion + 系统字体
    // 目的：与 Windows 版界面一致（见 docs/06-macos-port.md §9.1）
    void ApplyLook();

    // 把字符串包成 shell 单引号字面量，路径里有空格也不会散
    QString ShellQuote(const QString &s);

    // 转义成 AppleScript 双引号字符串的内容（do shell script "..."）
    QString AppleScriptString(const QString &s);

} // namespace NekoGui_mac
