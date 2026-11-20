#include <csignal>
#include <unistd.h>

#include <QApplication>
#include <QDir>
#include <QTranslator>
#include <QMessageBox>
#include <QStandardPaths>
#include <QLocalSocket>
#include <QLocalServer>
#include <QLibraryInfo>
#include <QSocketNotifier>
#include <QThread>

#include "3rdparty/RunGuard.hpp"
#include "main/NekoGui.hpp"
#include "3rdparty/qv2ray/v2/components/proxy/QvProxyConfigurator.hpp"

#include "ui/mainwindow_interface.h"

#ifdef Q_OS_WIN
#include "sys/windows/MiniDump.h"
#endif

#ifdef Q_OS_MACOS
#include "sys/macos/MacPlatform.hpp"
#endif

void signal_handler(int signum) {
    if (qApp) {
        GetMainWindow()->on_commitDataRequest();
        qApp->exit();
    }
}

#ifdef Q_OS_MACOS
// macOS 上 SIGTERM/SIGINT 可能在 Qt 或 CFRunLoop 内部被投递，直接在 handler 里调
// Qt（保存配置、退出事件循环）会破坏堆——实测退出时报 "malloc: Double free"，
// 保存的数据也可能被写坏。自管道（self-pipe）是标准做法：handler 里只 write 一个字节，
// 真正的处理搬到事件循环线程。
static int g_sigpipe[2] = {-1, -1};

static void mac_signal_relay(int) {
    if (g_sigpipe[1] >= 0) {
        const char c = 1;
        (void) write(g_sigpipe[1], &c, 1); // async-signal-safe
    }
}
#endif

QTranslator* trans = nullptr;
QTranslator* trans_qt = nullptr;

void loadTranslate(const QString& locale) {
    if (trans != nullptr) {
        trans->deleteLater();
    }
    if (trans_qt != nullptr) {
        trans_qt->deleteLater();
    }
    //
    trans = new QTranslator;
    trans_qt = new QTranslator;
    QLocale::setDefault(QLocale(locale));
    //
    if (trans->load(":/translations/" + locale + ".qm")) {
        QCoreApplication::installTranslator(trans);
    }
    // Qt 自带的 qtbase_*.qm 位置随平台不同：
    //   Windows/Linux: applicationDirPath()（部署脚本会拷进去）
    //   macOS bundle:  Contents/Resources/translations（macdeployqt 的布局）
    // 所以按候选目录依次找，别写死一个。
    const QString qtQm = "qtbase_" + locale + ".qm";
    const QStringList candidates{
            QLibraryInfo::path(QLibraryInfo::TranslationsPath),
            QApplication::applicationDirPath() + "/../Resources/translations",
            QApplication::applicationDirPath(),
    };
    for (const auto& dir: candidates) {
        if (dir.isEmpty()) continue;
        if (trans_qt->load(QDir(dir).absoluteFilePath(qtQm))) {
            QCoreApplication::installTranslator(trans_qt);
            break;
        }
    }
}

#define LOCAL_SERVER_PREFIX "rekoboxlocalserver-"

int main(int argc, char* argv[]) {
    // Core dump
#ifdef Q_OS_WIN
    Windows_SetCrashHandler();
#endif

    // pre-init QApplication
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    QApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif
    QApplication::setQuitOnLastWindowClosed(false);
    auto preQApp = new QApplication(argc, argv);

    // Clean
    QDir::setCurrent(QApplication::applicationDirPath());
    if (QFile::exists("updater.old")) {
        QFile::remove("updater.old");
    }
#ifndef Q_OS_WIN
#ifndef Q_OS_MACOS
    // Linux 用 launcher/updater 脚本做自动更新。
    // macOS 没有 launcher，建这个链接会变成 **断链符号链接**，直接破坏
    // codesign --deep 的校验（真机 Gatekeeper 会报"已损坏"），所以不做。
    if (!QFile::exists("updater")) {
        QFile::link("launcher", "updater");
    }
#endif
#endif

    // Flags
    NekoGui::dataStore->argv = QApplication::arguments();
    if (NekoGui::dataStore->argv.contains("-many")) NekoGui::dataStore->flag_many = true;
    if (NekoGui::dataStore->argv.contains("-appdata")) {
        NekoGui::dataStore->flag_use_appdata = true;
        int appdataIndex = NekoGui::dataStore->argv.indexOf("-appdata");
        if (NekoGui::dataStore->argv.size() > appdataIndex + 1 && !NekoGui::dataStore->argv.at(appdataIndex + 1).startsWith("-")) {
            NekoGui::dataStore->appdataDir = NekoGui::dataStore->argv.at(appdataIndex + 1);
        }
    }
    if (NekoGui::dataStore->argv.contains("-tray")) NekoGui::dataStore->flag_tray = true;
    if (NekoGui::dataStore->argv.contains("-debug")) NekoGui::dataStore->flag_debug = true;
    if (NekoGui::dataStore->argv.contains("-flag_restart_tun_on")) NekoGui::dataStore->flag_restart_tun_on = true;
    if (NekoGui::dataStore->argv.contains("-flag_reorder")) NekoGui::dataStore->flag_reorder = true;
#ifdef NKR_CPP_USE_APPDATA
    NekoGui::dataStore->flag_use_appdata = true; // Example: Package & MacOS
#endif
#ifdef NKR_CPP_DEBUG
    NekoGui::dataStore->flag_debug = true;
#endif

    // dirs & clean
    auto wd = QDir(QApplication::applicationDirPath());
    if (NekoGui::dataStore->flag_use_appdata) {
        QApplication::setApplicationName("rekobox");
        if (!NekoGui::dataStore->appdataDir.isEmpty()) {
            wd.setPath(NekoGui::dataStore->appdataDir);
        } else {
#ifdef Q_OS_MACOS
            // AppConfigLocation 在 macOS 上是 ~/Library/Preferences，不能放数据
            wd.setPath(NekoGui_mac::DataDir());
#else
            wd.setPath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
#endif
        }
    }
    if (!wd.exists()) wd.mkpath(wd.absolutePath());
    if (!wd.exists("config")) wd.mkdir("config");
    QDir::setCurrent(wd.absoluteFilePath("config"));
    QDir("temp").removeRecursively();

    // init QApplication
    delete preQApp;
    QApplication a(argc, argv);

#ifdef Q_OS_MACOS
    // 固定 Fusion + 系统字体：与 Windows 版观感一致（详见 docs/06-macos-port.md §9.1）
    NekoGui_mac::ApplyLook();
#endif

    // 上次被强杀（kill -9 / 崩溃）会留下指向 127.0.0.1 的系统代理，整机表现为断网。
    // 这一步必须在开窗口之前跑完：清干净再让用户看到"已连接"。
    RecoverSystemProxyIfNeeded();

#ifdef Q_OS_MACOS
    // TUN 模式会把系统 DNS 指向 tun 地址；崩在 TUN 开着的时候，这一步负责救回来
    NekoGui_mac::RecoverTunDNSIfNeeded();
#endif

    // dispatchers
    DS_cores = new QThread;
    DS_cores->start();

    // 单实例
    quint64 guard_data_in = GetRandomUint64();
#ifdef Q_OS_MACOS
    // mac 上不用 RunGuard（QSharedMemory）：进程被信号杀死（崩溃/SIGTERM/强杀）时
    // 共享内存段不会自动释放，之后每次启动都会判定"已有实例在跑"，唤醒又连不上，
    // 于是静默 return 0 —— 用户看到的现象是"点了图标打不开"（2026-11-20 实测）。
    // 改用固定名字的 QLocalServer 加锁：套接字文件在崩溃后是残留物，可以安全清掉。
    static QLocalServer mac_instance_lock;
    if (!NekoGui::dataStore->flag_many) {
        const auto lock_name = QStringLiteral(LOCAL_SERVER_PREFIX "lock");
        QLocalSocket probe;
        probe.connectToServer(lock_name);
        if (probe.waitForConnected(400)) {
            probe.write("raise");
            probe.waitForBytesWritten(300);
            probe.disconnectFromServer();
            qDebug() << "another instance is running, asked it to raise the window";
            return 0;
        }
        QLocalServer::removeServer(lock_name); // 清掉上次崩溃留下的套接字文件
        if (!mac_instance_lock.listen(lock_name)) {
            qWarning() << "failed to create single-instance lock:" << mac_instance_lock.errorString();
        }
        QObject::connect(&mac_instance_lock, &QLocalServer::newConnection, &a, [&] {
            while (auto conn = mac_instance_lock.nextPendingConnection()) {
                conn->deleteLater();
                MW_dialog_message("", "Raise"); // 第二个实例让我们把窗口抬起来
            }
        });
    }
    MF_release_runguard = [] {};
#else
    RunGuard guard("rekobox" + wd.absolutePath());
    quint64 guard_data_out = 0;
    if (!NekoGui::dataStore->flag_many && !guard.tryToRun(&guard_data_in)) {
        // Some Good System
        if (guard.isAnotherRunning(&guard_data_out)) {
            // Wake up a running instance
            QLocalSocket socket;
            socket.connectToServer(LOCAL_SERVER_PREFIX + Int2String(guard_data_out));
            qDebug() << socket.fullServerName();
            if (!socket.waitForConnected(500)) {
                qDebug() << "Failed to wake a running instance.";
                return 0;
            }
            qDebug() << "connected to local server, try to raise another program";
            return 0;
        }
        // Some Bad System
        QMessageBox::warning(nullptr, "NekoGui", "RunGuard disallow to run, use -many to force start.");
        return 0;
    }
    MF_release_runguard = [&] { guard.release(); };
#endif

// icons
#if (QT_VERSION >= QT_VERSION_CHECK(5, 11, 0))
    QIcon::setFallbackSearchPaths(QStringList{
        ":/neko",
        ":/icon",
    });
#endif

    // icon for no theme
    if (QIcon::themeName().isEmpty()) {
        QIcon::setThemeName("breeze");
    }

    // Dir
    QDir dir;
    bool dir_success = true;
    if (!dir.exists("profiles")) {
        dir_success &= dir.mkdir("profiles");
    }
    if (!dir.exists("groups")) {
        dir_success &= dir.mkdir("groups");
    }
    if (!dir.exists(ROUTES_PREFIX_NAME)) {
        dir_success &= dir.mkdir(ROUTES_PREFIX_NAME);
    }
    if (!dir_success) {
        QMessageBox::warning(nullptr, "Error", "No permission to write " + dir.absolutePath());
        return 1;
    }

    // Load dataStore
    switch (NekoGui::coreType) {
        case NekoGui::CoreType::SING_BOX:
            NekoGui::dataStore->fn = "groups/rekobox.json";
            break;
        default:
            MessageBoxWarning("Error", "Unknown coreType.");
            return 0;
    }
    auto isLoaded = NekoGui::dataStore->Load();
    if (!isLoaded) {
        NekoGui::dataStore->Save();
    }

    // Datastore & Flags
    if (NekoGui::dataStore->start_minimal) NekoGui::dataStore->flag_tray = true;

#ifdef Q_OS_MACOS
    // "Internal Tun"（主内核自己加 tun 入站）要求进程本身是 root，而 mac 上永远不是。
    // 打开它只会得到 "configure tun interface: Connect: operation not permitted"。
    // mac 必须走两进程模型：主内核管 socks，另起一个 root 内核建 utun。
    // 老配置里可能存着 true，这里强制纠正。
    NekoGui::dataStore->vpn_internal_tun = false;
#endif

    // load routing
    NekoGui::dataStore->routing = std::make_unique<NekoGui::Routing>();
    NekoGui::dataStore->routing->fn = ROUTES_PREFIX + NekoGui::dataStore->active_routing;
    isLoaded = NekoGui::dataStore->routing->Load();
    if (!isLoaded) {
        NekoGui::dataStore->routing->Save();
    }

    // Translate
    QString locale;
    switch (NekoGui::dataStore->language) {
        case 1: // English
            break;
        case 2:
            locale = "zh_CN";
            break;
        case 3:
            locale = "fa_IR"; // farsi(iran)
            break;
        case 4:
            locale = "ru_RU"; // Russian
            break;
        default:
            locale = QLocale().name();
    }
    QGuiApplication::tr("QT_LAYOUT_DIRECTION");
    loadTranslate(locale);

    // Signals
#ifdef Q_OS_MACOS
    if (pipe(g_sigpipe) == 0) {
        signal(SIGTERM, mac_signal_relay);
        signal(SIGINT, mac_signal_relay);
        auto sig_notifier = new QSocketNotifier(g_sigpipe[0], QSocketNotifier::Read, &a);
        QObject::connect(sig_notifier, &QSocketNotifier::activated, &a, [&a] {
            char c = 0;
            (void) read(g_sigpipe[0], &c, 1);
            qDebug() << "signal received, shutting down from the event loop";
            signal_handler(0); // 此刻已经在事件循环线程里，调 Qt 是安全的
        });
    } else {
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);
    }
#else
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
#endif

    // QLocalServer
    QLocalServer server;
    auto server_name = LOCAL_SERVER_PREFIX + Int2String(guard_data_in);
    QLocalServer::removeServer(server_name);
    server.listen(server_name);
    QObject::connect(&server, &QLocalServer::newConnection, &a, [&] {
        auto socket = server.nextPendingConnection();
        qDebug() << "nextPendingConnection:" << server_name << socket;
        socket->deleteLater();
        // raise main window
        MW_dialog_message("", "Raise");
    });

    UI_InitMainWindow();
    return QApplication::exec();
}
