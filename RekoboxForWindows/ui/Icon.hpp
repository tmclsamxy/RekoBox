#pragma once

#include <QIcon>
#include <QPixmap>

namespace Icon {

    enum TrayIconStatus {
        NONE,
        RUNNING,
        SYSTEM_PROXY,
        VPN,
    };

    QPixmap GetTrayIcon(TrayIconStatus status);

    // 托盘用：macOS 返回菜单栏模板图（自动适配深浅色），其他平台等价于 QIcon(GetTrayIcon())
    QIcon GetTrayQIcon(TrayIconStatus status);

    QPixmap GetMaterialIcon(const QString &name);

} // namespace Icon
