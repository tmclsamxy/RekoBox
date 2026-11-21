#include "main/AppLog.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>
#include <QStringList>

#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_MACOS
#include <execinfo.h>
#include <unistd.h>
#endif

namespace {
    QMutex g_mutex;
    QFile g_file;
    QString g_path;
    // 环形缓冲：崩溃时要能回看崩溃前发生了什么，所以最近 200 行始终留在内存里
    QStringList g_recent;
    constexpr int kRecentMax = 200;

    QtMessageHandler g_prev_handler = nullptr;

    QString LogDir() {
#if defined(Q_OS_MACOS)
        // ~/Library/Logs/Rekobox —— mac 上放这里才会出现在「控制台」App 的侧栏
        return QDir::homePath() + "/Library/Logs/Rekobox";
#elif defined(Q_OS_WIN)
        // 数据目录下的 logs/（main.cpp 已把 cwd 设成 config 目录，取其父）
        return QDir(QDir::currentPath()).absoluteFilePath("../logs");
#else
        const auto state = qEnvironmentVariable("XDG_STATE_HOME");
        const auto base = state.isEmpty() ? QDir::homePath() + "/.local/state" : state;
        return base + "/Rekobox";
#endif
    }

    const char *LevelName(QtMsgType t) {
        switch (t) {
            case QtDebugMsg: return "DEBUG";
            case QtInfoMsg: return "INFO";
            case QtWarningMsg: return "WARN";
            case QtCriticalMsg: return "ERROR";
            case QtFatalMsg: return "FATAL";
        }
        return "?";
    }

    // 超过 8MB 就轮转一份 app.log.1，避免长期运行把磁盘写满
    void RotateIfNeeded() {
        if (!g_file.exists()) return;
        if (g_file.size() < 8 * 1024 * 1024) return;
        const auto old = g_path + ".1";
        QFile::remove(old);
        g_file.close();
        QFile::rename(g_path, old);
        g_file.setFileName(g_path);
        g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }

    void WriteCrashReport() {
#ifdef Q_OS_MACOS
        const auto ts = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        const auto path = LogDir() + "/crash-" + ts + ".log";
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
        QTextStream s(&f);
        s << "RekoBox crash report\n";
        s << "time: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
        s << "version: " << QCoreApplication::applicationVersion() << "\n\n";
        s << "--- last log lines ---\n";
        for (const auto &l: g_recent) s << l << "\n";
        s << "\n--- backtrace ---\n";
        void *frames[64];
        const int n = backtrace(frames, 64);
        // backtrace_symbols 会 malloc：崩溃路径上用它不理想，但比没有强；
        // 真正的符号化要靠 dSYM + lldb（见排查报告 P0-2）。
        char **syms = backtrace_symbols(frames, n);
        if (syms != nullptr) {
            for (int i = 0; i < n; i++) s << syms[i] << "\n";
            free(syms);
        }
        s.flush();
        f.close();
#endif
    }

    void Handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
        // 先过一遍上一个 handler（默认那个），保证终端里仍然能看到
        if (g_prev_handler != nullptr) {
            g_prev_handler(type, ctx, msg);
        }

        const auto line = QString("[%1] [%2] %3")
                                  .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"),
                                       LevelName(type), msg);

        QMutexLocker lock(&g_mutex);
        g_recent << line;
        while (g_recent.size() > kRecentMax) g_recent.removeFirst();

        if (!g_file.isOpen()) return;
        if (type == QtFatalMsg) WriteCrashReport(); // 先把现场留住再写这一行
        RotateIfNeeded();
        QTextStream ts(&g_file);
        ts << line << "\n";
        ts.flush();
    }

    void SeedRecent() {
        g_recent << QString("[%1] [INFO] ---- RekoBox %2 starting ----")
                            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"),
                                 QCoreApplication::applicationVersion());
    }
} // namespace

namespace NekoGui_log {

    void Install() {
        QMutexLocker lock(&g_mutex);

        const auto dir = LogDir();
        QDir().mkpath(dir);
        g_path = dir + "/app.log";

        g_file.setFileName(g_path);
        if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            // 打不开（只读磁盘/权限问题）就静默退回 stderr，不要因为日志把 App 拖死
            g_path.clear();
            return;
        }
        SeedRecent();

        // 记一行启动信息：版本、架构、数据目录。
        // 排查时"用户装的是哪一版、是不是 arm64"是第一个要问的问题。
        const auto startup = QString("---- RekoBox %1 starting · %2 · %3 ----")
                                     .arg(QCoreApplication::applicationVersion(),
                                          QSysInfo::prettyProductName(),
                                          QSysInfo::currentCpuArchitecture());
        QTextStream(&g_file) << "\n"
                             << startup << "\n";
        g_recent << startup;

        g_prev_handler = qInstallMessageHandler(Handler);
    }

    QString LogFilePath() {
        QMutexLocker lock(&g_mutex);
        return g_path;
    }

    void Flush() {
        QMutexLocker lock(&g_mutex);
        if (g_file.isOpen()) g_file.flush();
    }

} // namespace NekoGui_log