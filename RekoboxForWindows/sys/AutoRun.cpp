#include "AutoRun.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>

#include "3rdparty/fix_old_qt.h"
#include "main/NekoGui.hpp"

// macOS：登录项走 SMAppService（ObjC++，见 sys/macos/AutoRunMac.mm）
#if defined(Q_OS_MACOS)
#include "sys/macos/AutoRunMac.hpp"
#endif

#ifdef Q_OS_WIN

#include <QSettings>

QString Windows_GenAutoRunString() {
    auto appPath = QApplication::applicationFilePath();
    appPath = "\"" + QDir::toNativeSeparators(appPath) + "\"";
    appPath += " -tray";
    return appPath;
}

bool AutoRun_SetEnabled(bool enable) {
    // 以程序名称作为注册表中的键
    // 根据键获取对应的值（程序路径）
    auto appPath = QApplication::applicationFilePath();
    QFileInfo fInfo(appPath);
    QString name = fInfo.baseName();

    QSettings settings("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);

    if (enable) {
        settings.setValue(name, Windows_GenAutoRunString());
    } else {
        settings.remove(name);
    }
    // 写注册表几乎不会失败；返回 status 复核结果，保证"设置成功"= 真的已启用
    return settings.status() == QSettings::NoError;
}

bool AutoRun_IsEnabled() {
    QSettings settings("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);

    // 以程序名称作为注册表中的键
    // 根据键获取对应的值（程序路径）
    auto appPath = QApplication::applicationFilePath();
    QFileInfo fInfo(appPath);
    QString name = fInfo.baseName();

    return settings.value(name).toString() == Windows_GenAutoRunString();
}

int AutoRun_Status() {
    // Windows 只有"写了 / 没写"两态，没有待批准一说
    return AutoRun_IsEnabled() ? 1 : 0;
}

#endif

#ifdef Q_OS_MACOS

// 为什么整段换掉而不是继续修 LSSharedFileList：
//
//   2026-11-20 本机实测（macOS 15.7.5 / arm64）：调用 LSSharedFileListInsertItemURL
//   **直接返回 NULL**，登录项列表里从来不会出现我们这一项（snapshot 前后都是同样条目）。
//   也就是说旧实现的失效原因不是"写入成功但系统不认"（报告里的推测），
//   而是整个 API 已经不能用了——只修 CFRelease 泄漏，功能仍然是坏的。
//
// 改用 macOS 13+ 的 SMAppService.mainAppService（实现在 sys/macos/AutoRunMac.mm）：
//   不需要 root、不需要特权助手，App 自己登进「系统设置 → 通用 → 登录项」。
//
// 与 Windows 的差异：mainAppService 拉起 App 时**不能带命令行参数**，所以无法照抄
// Windows 的 "…\rekobox.exe" -tray。开机静默进托盘改由 start_minimal 表达：
// 勾上自启时顺手把它打开，效果等同于 Windows 的 -tray（不弹主窗口，只留托盘图标）。

// 返回 false 表示"没设置成功"，调用方应当把勾选框复位并提示原因。
// SMAppService 的 mainAppService 只认**位于 /Applications** 的 App：
// 直接跑 RekoBoxForMac/build/RekoBox.app（开发调试）时状态恒为 NotFound，
// 这时勾选框会出现"勾了又跳回去"——要在界面上说清原因，别让它看起来像 bug。
bool AutoRun_SetEnabled(bool enable) {
    // 登录项传不了参数 → 用"启动时不显示面板"这条设置替代 Windows 的 -tray。
    // 只在开启时改，关掉自启不反向覆盖用户的界面偏好。
    if (enable && !NekoGui::dataStore->start_minimal) {
        NekoGui::dataStore->start_minimal = true;
        NekoGui::dataStore->Save();
        qDebug() << "[mac] autorun enabled, start_minimal -> true (mainAppService takes no args)";
    }

    if (!NekoGui_mac::AutoRun_SetEnabled(enable)) {
        qDebug() << "[mac] autorun set failed, status:" << NekoGui_mac::AutoRun_Status();
        return false;
    }

    // 系统可能返回 RequiresApproval：登记成功，但要用户在系统设置里点一下才生效。
    // 这种情况算"设置成功"（勾选框保持勾上），提示由界面层负责。
    return true;
}

// 0 = 没开；1 = 已开；2 = 已登记但需用户在系统设置里批准。
// 原来只返回 bool，把"需批准"和"未开"混为一谈，于是界面在两种情况下都显示未勾选。
int AutoRun_Status() {
    switch (NekoGui_mac::AutoRun_Status()) {
        case NekoGui_mac::AutoRun_Enabled:
            return 1;
        case NekoGui_mac::AutoRun_RequiresApproval:
            return 2;
        default:
            return 0;
    }
}

bool AutoRun_IsEnabled() {
    // RequiresApproval 也算"用户选了自启"（只是还没在系统设置里批准），
    // 勾选框要保持勾上，否则又会出现"勾了会跳回去"的观感。
    return NekoGui_mac::AutoRun_IsEnabled();
}

#endif

#ifdef Q_OS_LINUX

#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QTextStream>

#define NEWLINE "\n"

//  launchatlogin.cpp
//  ShadowClash
//
//  Created by TheWanderingCoel on 2018/6/12.
//  Copyright © 2019 Coel Wu. All rights reserved.
//
QString getUserAutostartDir_private() {
    QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    config += QLatin1String("/autostart/");
    return config;
}

bool AutoRun_SetEnabled(bool enable) {
    // From https://github.com/nextcloud/desktop/blob/master/src/common/utility_unix.cpp
    QString appName = QCoreApplication::applicationName();
    QString userAutoStartPath = getUserAutostartDir_private();
    QString desktopFileLocation = userAutoStartPath + appName + QLatin1String(".desktop");
    QStringList appCmdList;

    // nekoray: launcher
    if (qEnvironmentVariable("NKR_FROM_LAUNCHER") == "1") {
        appCmdList << QApplication::applicationDirPath() + "/launcher"
                   << "--";
    } else {
        if (QProcessEnvironment::systemEnvironment().contains("APPIMAGE")) {
            appCmdList << QProcessEnvironment::systemEnvironment().value("APPIMAGE");
        } else {
            appCmdList << QApplication::applicationFilePath();
        }
    }

    appCmdList << "-tray";

    if (NekoGui::dataStore->flag_use_appdata) {
        appCmdList << "-appdata";
    }

    if (enable) {
        if (!QDir().exists(userAutoStartPath) && !QDir().mkpath(userAutoStartPath)) {
            // qCWarning(lcUtility) << "Could not create autostart folder"
            // << userAutoStartPath;
            return false;
        }

        QFile iniFile(desktopFileLocation);

        if (!iniFile.open(QIODevice::WriteOnly)) {
            // qCWarning(lcUtility) << "Could not write auto start entry" <<
            // desktopFileLocation;
            return false;
        }

        QTextStream ts(&iniFile);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        ts.setCodec("UTF-8");
#endif
        ts << QLatin1String("[Desktop Entry]") << NEWLINE
           << QLatin1String("Name=") << appName << NEWLINE
           << QLatin1String("Exec=") << appCmdList.join(" ") << NEWLINE
           << QLatin1String("Terminal=") << "false" << NEWLINE
           << QLatin1String("Categories=") << "Network" << NEWLINE
           << QLatin1String("Type=") << "Application" << NEWLINE
           << QLatin1String("StartupNotify=") << "false" << NEWLINE
           << QLatin1String("X-GNOME-Autostart-enabled=") << "true" << NEWLINE;
        ts.flush();
        iniFile.close();
        return QFile::exists(desktopFileLocation);
    } else {
        QFile::remove(desktopFileLocation);
        return !QFile::exists(desktopFileLocation);
    }
}

bool AutoRun_IsEnabled() {
    QString appName = QCoreApplication::applicationName();
    QString desktopFileLocation = getUserAutostartDir_private() + appName + QLatin1String(".desktop");
    return QFile::exists(desktopFileLocation);
}

int AutoRun_Status() {
    // Linux 只有"文件在 / 不在"两态
    return AutoRun_IsEnabled() ? 1 : 0;
}

#endif
