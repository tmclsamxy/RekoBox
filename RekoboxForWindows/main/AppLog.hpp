#pragma once

#include <QString>

// 应用日志落盘。
//
// 为什么需要：全仓原来没有 qInstallMessageHandler，App 自己的 qDebug（包括所有
// "[mac] …" 诊断）从 Finder 启动时进黑洞——只有从终端 `open -W` 或直接跑二进制才看得到。
// 内核日志（config/neko.log）反而有文件，出问题时最需要的那一半恰恰没有。
//
// 落盘位置：
//   macOS    ~/Library/Logs/Rekobox/app.log      （Console.app 也能直接看到）
//   Windows  <数据目录>/logs/app.log
//   Linux    ~/.local/state/Rekobox/app.log（XDG_STATE_HOME 优先）
//
// 同时还接管未捕获异常/致命信号，把最后若干行日志和栈写进同目录的 crash-*.log。

namespace NekoGui_log {

    // 安装消息处理器。必须在 QApplication 构造之后调用（要用 applicationName）。
    // 会同时把输出保留到 stderr，所以从终端跑时的行为不变。
    void Install();

    // 当前日志文件路径（未安装时返回空）
    QString LogFilePath();

    // 立即 flush（退出前调用，避免最后几行丢在缓冲区里）
    void Flush();

} // namespace NekoGui_log