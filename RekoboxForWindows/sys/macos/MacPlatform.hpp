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

    // TUN 模式下接管系统 DNS。
    // 背景：sing-tun 在 darwin 上不会给 utun 下发 DNS（Linux/Windows 才会），
    // 不接管的话域名仍由路由器解析——既可能被污染，也是明文泄漏。
    // enable=true：把各网络服务的 DNS 指向 tun 地址，并把原值落盘（tun-dns-state.json）；
    // enable=false：按快照还原（原本是 DHCP 的要还原成 Empty，不是写死某个 IP）。
    void ApplyTunDNS(bool enable);

    // 启动时清理上次异常退出（崩溃 / 强杀）留下的 DNS 改动，否则整机解析不了域名
    void RecoverTunDNSIfNeeded();

} // namespace NekoGui_mac
