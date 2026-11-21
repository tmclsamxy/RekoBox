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

    // TUN 模式下接管系统 DNS —— 照安卓的做法（VpnService.addDnsServer 用的就是
    // 172.19.0.2，即 PRIVATE_VLAN4_ROUTER）：把各网络服务的 DNS 指向 tun 网段里的
    // 这个地址，查询会进 utun，由 tun 配置里 "port 53 → dns-out" 规则劫持。
    //
    // 为什么不用 tun 自己的地址 172.19.0.1：那是接口地址，内核本地投递，包不进 utun。
    // 为什么 172.19.0.2 能行：它只是网段里的普通地址，走默认路由进 tun。
    // **前提**：route_exclude_address 里不能包含 172.16.0.0/12（曾踩过，被排除后就不进 tun 了）。
    //
    // 硬编码公共 DNS（8.8.8.8 之类）不靠这个，由 tun 路由直接捕获，两条路都覆盖。
    // enable=true 时落盘快照后改；false 按快照还原（原本 DHCP 的还原成 Empty）。
    void ApplyTunDNS(bool enable);

    // 指定接管地址的版本：TUN 内核监听 127.0.0.1:53 之后，这里传 "127.0.0.1"。
    // 保留 address 参数是为了兼容旧快照还原与灰度（早期版本写的是 172.19.0.2）。
    void ApplyTunDNS(bool enable, const QString &address);

    // 启动时清理上次异常退出（崩溃 / 强杀）留下的 DNS 改动，否则整机解析不了域名
    void RecoverTunDNSIfNeeded();

    // 探测 server:port 有没有 DNS 在应答（最小查询 + 比对事务 ID）。
    // 切换系统 DNS 前必须探测通过，否则会有一段"DNS 指向没人监听的地址"→整机解析挂掉。
    bool ProbeDNS(const QString &server, int port, int timeoutMs = 900);

    // 取一个"真实可用"的上游 DNS，给 tun 配置的 dns-direct / dns-in override 用。
    // 顺序：显式配置的 DNS > 系统当前解析器 > 223.5.5.5。
    // 绝不能返回 127.0.0.1 —— 接管后那就是它自己，会自环。
    QString TunDNSUpstream();

    // 「从屏幕导入二维码」需要"屏幕录制"权限（macOS 10.15+）。没有权限时
    // QScreen::grabWindow 不报错，只是返回一张**不含任何窗口内容**的图（只剩壁纸），
    // 于是二维码永远识别不到、每次都弹 "QR Code not found"。
    // HasScreenCaptureAccess 只查询不弹框（CGPreflightScreenCaptureAccess）；
    // RequestScreenCaptureAccess 会弹系统授权框（CGRequestScreenCaptureAccess），
    // 且授权后必须重启进程才生效——所以调用方要先提示用户。
    bool HasScreenCaptureAccess();

    bool RequestScreenCaptureAccess();

    // ------------------------------------------------------------------
    // TUN 停止握手（P0-3：不再二次弹管理员密码）
    //
    // res/vpn/vpn-run-root-macos.sh 以 root 起来后自己盯一个哨兵循环：
    //   config/tun-root.pid        当前 tun 内核 PID
    //   config/tun-stop-request    GUI 建这个文件 = "请停"
    // 哨兵是 root，所以它能 SIGTERM 内核；GUI 建文件不需要提权。
    //
    // RequestTunStopBySentinel 只负责投递请求（不阻塞、不提权）。
    // WaitTunExit 轮询 pid 文件消失，确认内核真的退了（脚本里 SIGTERM→6 秒→SIGKILL）。
    // 两者配合就只有"启动"那一次密码，对齐 Windows 的体验。
    // ------------------------------------------------------------------

    qint64 ReadTunRootPid();

    bool RequestTunStopBySentinel();

    bool WaitTunExit(int timeoutMs);

} // namespace NekoGui_mac
