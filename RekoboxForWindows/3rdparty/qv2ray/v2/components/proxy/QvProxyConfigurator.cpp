#include "QvProxyConfigurator.hpp"

#ifdef Q_OS_WIN
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
//
#include <wininet.h>
#include <ras.h>
#include <raserror.h>
#include <vector>
#endif

#include <QStandardPaths>
#include <QProcess>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include "3rdparty/fix_old_qt.h"
#include "3rdparty/qv2ray/wrapper.hpp"
#include "fmt/Preset.hpp"
#include "main/NekoGui.hpp"

#define QV_MODULE_NAME "SystemProxy"

#define QSTRN(num) QString::number(num)

namespace Qv2ray::components::proxy {

    using ProcessArgument = QPair<QString, QStringList>;
#ifdef Q_OS_MACOS
    QStringList macOSgetNetworkServices() {
        QProcess p;
        p.setProgram("/usr/sbin/networksetup");
        p.setArguments(QStringList{"-listallnetworkservices"});
        p.start();
        p.waitForStarted();
        p.waitForFinished();
        LOG(p.errorString());
        auto str = p.readAllStandardOutput();
        auto lines = SplitLines(str);
        QStringList result;

        // Start from 1 since first line is unneeded.
        for (auto i = 1; i < lines.count(); i++) {
            // * means disabled.
            if (!lines[i].contains("*")) {
                result << lines[i];
            }
        }

        LOG("Found " + QSTRN(result.size()) + " network services: " + result.join(";"));
        return result;
    }

    // ---------------------------------------------------------------- 状态快照
    //
    // networksetup 设置的代理写在系统配置里，进程被 kill -9 时没有任何回调机会，
    // 重启后残留的 127.0.0.1:2080 会让整机看起来"断网"（浏览器全部超时）。
    // 所以动系统代理之前，先把每个网络服务**原本的状态**落盘；退出（或下次启动）
    // 时按快照还原，而不是无脑全关——用户可能本来就挂着自己的公司代理。
    namespace {
        constexpr auto PROXY_STATE_FILE = "system-proxy-state.json";

        QString MacProxyStatePath() {
            // 运行期 cwd 就是 config 目录（main.cpp 里 setCurrent 过）
            auto dir = QDir::current();
            if (!dir.absolutePath().isEmpty() && dir.exists()) {
                return dir.absoluteFilePath(PROXY_STATE_FILE);
            }
            return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absoluteFilePath(PROXY_STATE_FILE);
        }

        QString MacNetSet(const QStringList &args) {
            QProcess p;
            p.start("/usr/sbin/networksetup", args);
            if (!p.waitForFinished(15000)) {
                p.kill();
                p.waitForFinished(1000);
                LOG("networksetup timeout: " + args.join(" "));
                return {};
            }
            if (p.exitCode() != 0) {
                LOG("networksetup failed(" + QSTRN(p.exitCode()) + "): " + args.join(" "));
            }
            return QString::fromUtf8(p.readAllStandardOutput());
        }

        // 从 "Enabled: Yes\nServer: 127.0.0.1\nPort: 2080" 里取字段
        QString MacField(const QString &out, const QString &key) {
            for (const auto &line: SplitLines(out)) {
                if (line.startsWith(key + ":")) {
                    return line.mid(key.length() + 1).trimmed();
                }
            }
            return {};
        }

        struct MacServiceProxy {
            QString service;
            bool web = false;
            int webPort = 0;
            QString webServer;
            bool secure = false;
            int securePort = 0;
            QString secureServer;
            bool socks = false;
            int socksPort = 0;
            QString socksServer;
            bool autoOn = false;
            QString autoUrl;
            QStringList bypass;

            QJsonObject ToJson() const {
                return QJsonObject{
                        {"service", service},
                        {"web", web}, {"web_port", webPort}, {"web_server", webServer},
                        {"secure", secure}, {"secure_port", securePort}, {"secure_server", secureServer},
                        {"socks", socks}, {"socks_port", socksPort}, {"socks_server", socksServer},
                        {"auto", autoOn}, {"auto_url", autoUrl},
                        {"bypass", QJsonArray::fromStringList(bypass)},
                };
            }

            static MacServiceProxy FromJson(const QJsonObject &o) {
                MacServiceProxy s;
                s.service = o["service"].toString();
                s.web = o["web"].toBool();
                s.webPort = o["web_port"].toInt();
                s.webServer = o["web_server"].toString();
                s.secure = o["secure"].toBool();
                s.securePort = o["secure_port"].toInt();
                s.secureServer = o["secure_server"].toString();
                s.socks = o["socks"].toBool();
                s.socksPort = o["socks_port"].toInt();
                s.socksServer = o["socks_server"].toString();
                s.autoOn = o["auto"].toBool();
                s.autoUrl = o["auto_url"].toString();
                for (const auto &v: o["bypass"].toArray()) s.bypass << v.toString();
                return s;
            }
        };

        MacServiceProxy MacCaptureService(const QString &service) {
            MacServiceProxy s;
            s.service = service;

            // 没配过的字段 networksetup 会回文字 "(null)"，当成空串存，
            // 否则还原时会被当成一个真的服务器地址写回去
            auto clean = [](QString v) {
                return v == "(null)" ? QString() : v;
            };

            auto web = MacNetSet({"-getwebproxy", service});
            s.web = MacField(web, "Enabled").compare("Yes", Qt::CaseInsensitive) == 0;
            s.webServer = clean(MacField(web, "Server"));
            s.webPort = MacField(web, "Port").toInt();

            auto secure = MacNetSet({"-getsecurewebproxy", service});
            s.secure = MacField(secure, "Enabled").compare("Yes", Qt::CaseInsensitive) == 0;
            s.secureServer = clean(MacField(secure, "Server"));
            s.securePort = MacField(secure, "Port").toInt();

            auto socks = MacNetSet({"-getsocksfirewallproxy", service});
            s.socks = MacField(socks, "Enabled").compare("Yes", Qt::CaseInsensitive) == 0;
            s.socksServer = clean(MacField(socks, "Server"));
            s.socksPort = MacField(socks, "Port").toInt();

            auto pac = MacNetSet({"-getautoproxyurl", service});
            s.autoOn = MacField(pac, "Enabled").compare("Yes", Qt::CaseInsensitive) == 0;
            s.autoUrl = clean(MacField(pac, "URL"));
            // 空的时候 networksetup 返回 "There aren't any bypass domains set on <service>."
            auto domains = MacNetSet({"-getproxybypassdomains", service});
            if (!domains.contains("aren't any")) {
                for (const auto &line: SplitLines(domains)) {
                    if (!line.trimmed().isEmpty()) s.bypass << line.trimmed();
                }
            }
            return s;
        }

        bool MacSaveState(const QList<MacServiceProxy> &list) {
            QJsonArray arr;
            for (const auto &s: list) arr.append(s.ToJson());
            QJsonObject root{{"services", arr}, {"since", QDateTime::currentDateTime().toString(Qt::ISODate)}};

            QFile f(MacProxyStatePath());
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                LOG("cannot write proxy state: " + f.errorString());
                return false;
            }
            f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            f.close();
            return true;
        }

        bool MacLoadState(QList<MacServiceProxy> &list) {
            QFile f(MacProxyStatePath());
            if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;
            auto doc = QJsonDocument::fromJson(f.readAll());
            f.close();
            if (!doc.isObject()) return false;
            for (const auto &v: doc.object()["services"].toArray()) {
                auto s = MacServiceProxy::FromJson(v.toObject());
                if (!s.service.isEmpty()) list << s;
            }
            return !list.isEmpty();
        }

        void MacRestoreService(const MacServiceProxy &s) {
            // 先把地址/端口写回去，再按"原本是开还是关"落开关。
            // 只写开关的话地址会留成我们的 2080，用户下次打开自己的代理客户端
            // 之前先看到一串错的值（而且 networksetup 改地址本身就会把开关打开，
            // 所以顺序必须是 地址 → 开关）。
            if (!s.webServer.isEmpty() && s.webPort > 0) {
                MacNetSet({"-setwebproxy", s.service, s.webServer, QSTRN(s.webPort)});
            }
            MacNetSet({"-setwebproxystate", s.service, s.web ? "on" : "off"});

            if (!s.secureServer.isEmpty() && s.securePort > 0) {
                MacNetSet({"-setsecurewebproxy", s.service, s.secureServer, QSTRN(s.securePort)});
            }
            MacNetSet({"-setsecurewebproxystate", s.service, s.secure ? "on" : "off"});

            if (!s.socksServer.isEmpty() && s.socksPort > 0) {
                MacNetSet({"-setsocksfirewallproxy", s.service, s.socksServer, QSTRN(s.socksPort)});
            }
            MacNetSet({"-setsocksfirewallproxystate", s.service, s.socks ? "on" : "off"});

            if (!s.autoUrl.isEmpty()) {
                MacNetSet({"-setautoproxyurl", s.service, s.autoUrl});
            }
            MacNetSet({"-setautoproxystate", s.service, s.autoOn ? "on" : "off"});

            if (s.bypass.isEmpty()) {
                MacNetSet({"-setproxybypassdomains", s.service, "Empty"});
            } else {
                MacNetSet(QStringList{"-setproxybypassdomains", s.service} + s.bypass);
            }
        }

        void MacApplyService(const QString &service, int httpPort, int socksPort, bool hasHTTP, bool hasSOCKS) {
            const QString address = "127.0.0.1";
            // 顺序：先设地址再开开关，反过来的话可能带着旧地址先启用
            if (hasHTTP) {
                MacNetSet({"-setwebproxy", service, address, QSTRN(httpPort)});
                MacNetSet({"-setwebproxystate", service, "on"});
                MacNetSet({"-setsecurewebproxy", service, address, QSTRN(httpPort)});
                MacNetSet({"-setsecurewebproxystate", service, "on"});
            }

            if (hasSOCKS) {
                MacNetSet({"-setsocksfirewallproxy", service, address, QSTRN(socksPort)});
                MacNetSet({"-setsocksfirewallproxystate", service, "on"});
            }

            // 本地/内网直连，否则访问自己的 NAS、打印机、localhost 都要绕一圈
            MacNetSet({"-setproxybypassdomains", service, "localhost", "127.0.0.1", "::1",
                       "*.local", "169.254/16", "192.168.*.*", "10.*.*.*", "172.16.*.*", "172.31.*.*"});
        }
    } // namespace
#endif
#ifdef Q_OS_WIN
#define NO_CONST(expr) const_cast<wchar_t *>(expr)
    // static auto DEFAULT_CONNECTION_NAME =
    // NO_CONST(L"DefaultConnectionSettings");
    ///
    /// INTERNAL FUNCTION
    bool __QueryProxyOptions() {
        INTERNET_PER_CONN_OPTION_LIST List;
        INTERNET_PER_CONN_OPTION Option[5];
        //
        unsigned long nSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
        Option[0].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
        Option[1].dwOption = INTERNET_PER_CONN_AUTODISCOVERY_FLAGS;
        Option[2].dwOption = INTERNET_PER_CONN_FLAGS;
        Option[3].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
        Option[4].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
        //
        List.dwSize = sizeof(INTERNET_PER_CONN_OPTION_LIST);
        List.pszConnection = nullptr; // NO_CONST(DEFAULT_CONNECTION_NAME);
        List.dwOptionCount = 5;
        List.dwOptionError = 0;
        List.pOptions = Option;

        if (!InternetQueryOption(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &List, &nSize)) {
            LOG("InternetQueryOption failed, GLE=" + QSTRN(GetLastError()));
        }

        LOG("System default proxy info:");

        if (Option[0].Value.pszValue != nullptr) {
            LOG(QString::fromWCharArray(Option[0].Value.pszValue));
        }

        if ((Option[2].Value.dwValue & PROXY_TYPE_AUTO_PROXY_URL) == PROXY_TYPE_AUTO_PROXY_URL) {
            LOG("PROXY_TYPE_AUTO_PROXY_URL");
        }

        if ((Option[2].Value.dwValue & PROXY_TYPE_AUTO_DETECT) == PROXY_TYPE_AUTO_DETECT) {
            LOG("PROXY_TYPE_AUTO_DETECT");
        }

        if ((Option[2].Value.dwValue & PROXY_TYPE_DIRECT) == PROXY_TYPE_DIRECT) {
            LOG("PROXY_TYPE_DIRECT");
        }

        if ((Option[2].Value.dwValue & PROXY_TYPE_PROXY) == PROXY_TYPE_PROXY) {
            LOG("PROXY_TYPE_PROXY");
        }

        if (!InternetQueryOption(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &List, &nSize)) {
            LOG("InternetQueryOption failed,GLE=" + QSTRN(GetLastError()));
        }

        if (Option[4].Value.pszValue != nullptr) {
            LOG(QString::fromStdWString(Option[4].Value.pszValue));
        }

        INTERNET_VERSION_INFO Version;
        nSize = sizeof(INTERNET_VERSION_INFO);
        InternetQueryOption(nullptr, INTERNET_OPTION_VERSION, &Version, &nSize);

        if (Option[0].Value.pszValue != nullptr) {
            GlobalFree(Option[0].Value.pszValue);
        }

        if (Option[3].Value.pszValue != nullptr) {
            GlobalFree(Option[3].Value.pszValue);
        }

        if (Option[4].Value.pszValue != nullptr) {
            GlobalFree(Option[4].Value.pszValue);
        }

        return false;
    }
    bool __SetProxyOptions(LPWSTR proxy_full_addr, bool isPAC) {
        INTERNET_PER_CONN_OPTION_LIST list;
        DWORD dwBufSize = sizeof(list);
        // Fill the list structure.
        list.dwSize = sizeof(list);
        // NULL == LAN, otherwise connectoid name.
        list.pszConnection = nullptr;

        if (nullptr == proxy_full_addr) {
            LOG("Clearing system proxy");
            //
            list.dwOptionCount = 1;
            list.pOptions = new INTERNET_PER_CONN_OPTION[1];

            // Ensure that the memory was allocated.
            if (nullptr == list.pOptions) {
                // Return if the memory wasn't allocated.
                return false;
            }

            // Set flags.
            list.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
            list.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT;
        } else if (isPAC) {
            LOG("Setting system proxy for PAC");
            //
            list.dwOptionCount = 2;
            list.pOptions = new INTERNET_PER_CONN_OPTION[2];

            if (nullptr == list.pOptions) {
                return false;
            }

            // Set flags.
            list.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
            list.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT | PROXY_TYPE_AUTO_PROXY_URL;
            // Set proxy name.
            list.pOptions[1].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
            list.pOptions[1].Value.pszValue = proxy_full_addr;
        } else {
            LOG("Setting system proxy for Global Proxy");
            //
            list.dwOptionCount = 2;
            list.pOptions = new INTERNET_PER_CONN_OPTION[2];

            if (nullptr == list.pOptions) {
                return false;
            }

            // Set flags.
            list.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
            list.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY;
            // Set proxy name.
            list.pOptions[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
            list.pOptions[1].Value.pszValue = proxy_full_addr;
            // Set proxy override.
            // list.pOptions[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
            // auto localhost = L"localhost";
            // list.pOptions[2].Value.pszValue = NO_CONST(localhost);
        }

        // Set proxy for LAN.
        if (!InternetSetOption(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, dwBufSize)) {
            LOG("InternetSetOption failed for LAN, GLE=" + QSTRN(GetLastError()));
        }

        RASENTRYNAME entry;
        entry.dwSize = sizeof(entry);
        std::vector<RASENTRYNAME> entries;
        DWORD size = sizeof(entry), count;
        LPRASENTRYNAME entryAddr = &entry;
        auto ret = RasEnumEntries(nullptr, nullptr, entryAddr, &size, &count);
        if (ERROR_BUFFER_TOO_SMALL == ret) {
            entries.resize(count);
            entries[0].dwSize = sizeof(RASENTRYNAME);
            entryAddr = entries.data();
            ret = RasEnumEntries(nullptr, nullptr, entryAddr, &size, &count);
        }
        if (ERROR_SUCCESS != ret) {
            LOG("Failed to list entry names");
            return false;
        }

        // Set proxy for each connectoid.
        for (DWORD i = 0; i < count; ++i) {
            list.pszConnection = entryAddr[i].szEntryName;
            if (!InternetSetOption(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, dwBufSize)) {
                LOG("InternetSetOption failed for connectoid " + QString::fromWCharArray(list.pszConnection) + ", GLE=" + QSTRN(GetLastError()));
            }
        }

        delete[] list.pOptions;
        InternetSetOption(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
        InternetSetOption(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
        return true;
    }
#endif

    void SetSystemProxy(int httpPort, int socksPort) {
        const QString &address = "127.0.0.1";
        bool hasHTTP = (httpPort > 0 && httpPort < 65536);
        bool hasSOCKS = (socksPort > 0 && socksPort < 65536);

#ifdef Q_OS_WIN
        if (!hasHTTP) {
            LOG("Nothing?");
            return;
        } else {
            LOG("Qv2ray will set system proxy to use HTTP");
        }
#else
        if (!hasHTTP && !hasSOCKS) {
            LOG("Nothing?");
            return;
        }

        if (hasHTTP) {
            LOG("Qv2ray will set system proxy to use HTTP");
        }

        if (hasSOCKS) {
            LOG("Qv2ray will set system proxy to use SOCKS");
        }
#endif

#ifdef Q_OS_WIN
        QString str = NekoGui::dataStore->system_proxy_format;
        if (str.isEmpty()) str = Preset::Windows::system_proxy_format[0];
        str = str.replace("{ip}", address)
                  .replace("{http_port}", Int2String(httpPort))
                  .replace("{socks_port}", Int2String(socksPort));
        //
        LOG("Windows proxy string: " + str);
        auto proxyStrW = new WCHAR[str.length() + 1];
        wcscpy(proxyStrW, str.toStdWString().c_str());
        //
        __QueryProxyOptions();

        if (!__SetProxyOptions(proxyStrW, false)) {
            LOG("Failed to set proxy.");
        }

        __QueryProxyOptions();
#elif defined(Q_OS_LINUX)
        QList<ProcessArgument> actions;
        actions << ProcessArgument{"gsettings", {"set", "org.gnome.system.proxy", "mode", "manual"}};
        //
        bool isKDE = qEnvironmentVariable("XDG_SESSION_DESKTOP") == "KDE" ||
                     qEnvironmentVariable("XDG_SESSION_DESKTOP") == "plasma";
        const auto configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);

        //
        // Configure HTTP Proxies for HTTP, FTP and HTTPS
        if (hasHTTP) {
            // iterate over protocols...
            for (const auto &protocol: QStringList{"http", "ftp", "https"}) {
                // for GNOME:
                {
                    actions << ProcessArgument{"gsettings",
                                               {"set", "org.gnome.system.proxy." + protocol, "host", address}};
                    actions << ProcessArgument{"gsettings",
                                               {"set", "org.gnome.system.proxy." + protocol, "port", QSTRN(httpPort)}};
                }

                // for KDE:
                if (isKDE) {
                    actions << ProcessArgument{"kwriteconfig5",
                                               {"--file", configPath + "/kioslaverc", //
                                                "--group", "Proxy Settings",          //
                                                "--key", protocol + "Proxy",          //
                                                "http://" + address + " " + QSTRN(httpPort)}};
                }
            }
        }

        // Configure SOCKS5 Proxies
        if (hasSOCKS) {
            // for GNOME:
            {
                actions << ProcessArgument{"gsettings", {"set", "org.gnome.system.proxy.socks", "host", address}};
                actions << ProcessArgument{"gsettings",
                                           {"set", "org.gnome.system.proxy.socks", "port", QSTRN(socksPort)}};

                // for KDE:
                if (isKDE) {
                    actions << ProcessArgument{"kwriteconfig5",
                                               {"--file", configPath + "/kioslaverc", //
                                                "--group", "Proxy Settings",          //
                                                "--key", "socksProxy",                //
                                                "socks://" + address + " " + QSTRN(socksPort)}};
                }
            }
        }
        // Setting Proxy Mode to Manual
        {
            // for GNOME:
            {
                actions << ProcessArgument{"gsettings", {"set", "org.gnome.system.proxy", "mode", "manual"}};
            }

            // for KDE:
            if (isKDE) {
                actions << ProcessArgument{"kwriteconfig5",
                                           {"--file", configPath + "/kioslaverc", //
                                            "--group", "Proxy Settings",          //
                                            "--key", "ProxyType", "1"}};
            }
        }

        // Notify kioslaves to reload system proxy configuration.
        if (isKDE) {
            actions << ProcessArgument{"dbus-send",
                                       {"--type=signal", "/KIO/Scheduler",                 //
                                        "org.kde.KIO.Scheduler.reparseSlaveConfiguration", //
                                        "string:''"}};
        }
        // Execute them all!
        //
        // note: do not use std::all_of / any_of / none_of,
        // because those are short-circuit and cannot guarantee atomicity.
        QList<bool> results;
        for (const auto &action: actions) {
            // execute and get the code
            const auto returnCode = QProcess::execute(action.first, action.second);
            // print out the commands and result codes
            DEBUG(QStringLiteral("[%1] Program: %2, Args: %3").arg(returnCode).arg(action.first).arg(action.second.join(";")));
            // give the code back
            results << (returnCode == QProcess::NormalExit);
        }

        if (results.count(true) != actions.size()) {
            LOG("Something wrong when setting proxies.");
        }
#elif defined(Q_OS_MACOS)
        auto services = macOSgetNetworkServices();

        // 只在"第一次改动前"记录原始状态。清单已存在说明就是我们改的，
        // 此时再抓一次会把 127.0.0.1:2080 当成用户的原始配置存起来，还原就失效了。
        if (!QFile::exists(MacProxyStatePath())) {
            QList<MacServiceProxy> snapshot;
            for (const auto &service: services) snapshot << MacCaptureService(service);
            MacSaveState(snapshot);
            LOG("system proxy state saved: " + MacProxyStatePath());
        } else {
            LOG("system proxy state already exists, keep the original snapshot");
        }

        for (const auto &service: services) {
            LOG("Setting proxy for interface: " + service);
            MacApplyService(service, httpPort, socksPort, hasHTTP, hasSOCKS);
        }

#endif
    }

    void ClearSystemProxy() {
        LOG("Clearing System Proxy");

#ifdef Q_OS_WIN
        if (!__SetProxyOptions(nullptr, false)) {
            LOG("Failed to clear proxy.");
        }
#elif defined(Q_OS_LINUX)
        QList<ProcessArgument> actions;
        const bool isKDE = qEnvironmentVariable("XDG_SESSION_DESKTOP") == "KDE" ||
                           qEnvironmentVariable("XDG_SESSION_DESKTOP") == "plasma";
        const auto configRoot = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);

        // Setting System Proxy Mode to: None
        {
            // for GNOME:
            {
                actions << ProcessArgument{"gsettings", {"set", "org.gnome.system.proxy", "mode", "none"}};
            }

            // for KDE:
            if (isKDE) {
                actions << ProcessArgument{"kwriteconfig5",
                                           {"--file", configRoot + "/kioslaverc", //
                                            "--group", "Proxy Settings",          //
                                            "--key", "ProxyType", "0"}};
            }
        }

        // Notify kioslaves to reload system proxy configuration.
        if (isKDE) {
            actions << ProcessArgument{"dbus-send",
                                       {"--type=signal", "/KIO/Scheduler",                 //
                                        "org.kde.KIO.Scheduler.reparseSlaveConfiguration", //
                                        "string:''"}};
        }

        // Execute the Actions
        for (const auto &action: actions) {
            // execute and get the code
            const auto returnCode = QProcess::execute(action.first, action.second);
            // print out the commands and result codes
            DEBUG(QStringLiteral("[%1] Program: %2, Args: %3").arg(returnCode).arg(action.first).arg(action.second.join(";")));
        }

#elif defined(Q_OS_MACOS)
        QList<MacServiceProxy> snapshot;
        if (MacLoadState(snapshot)) {
            // 按快照还原到"我们动手之前"的样子，而不是一律关掉
            for (const auto &s: snapshot) {
                LOG("Restoring proxy for interface: " + s.service);
                MacRestoreService(s);
            }
            QFile::remove(MacProxyStatePath());
            LOG("system proxy restored to the snapshot taken before we touched it");
        } else {
            // 没有快照（旧版本残留 / 文件被删）：保守地全部关掉，先保证能上网
            LOG("no proxy snapshot, turning all proxies off");
            for (const auto &service: macOSgetNetworkServices()) {
                LOG("Clearing proxy for interface: " + service);
                MacNetSet({"-setautoproxystate", service, "off"});
                MacNetSet({"-setwebproxystate", service, "off"});
                MacNetSet({"-setsecurewebproxystate", service, "off"});
                MacNetSet({"-setsocksfirewallproxystate", service, "off"});
            }
        }

#endif
    }

    void RecoverSystemProxyIfNeeded() {
#ifdef Q_OS_MACOS
        // 只有"非正常退出"才会留下清单：正常退出在 on_menu_exit_triggered 里已经还原并删掉了
        if (!QFile::exists(MacProxyStatePath())) return;
        LOG("leftover system proxy detected, restoring before startup");
        ClearSystemProxy();
#endif
    }
} // namespace Qv2ray::components::proxy
