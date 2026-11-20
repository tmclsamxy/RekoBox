#include "Icon.hpp"

#include "main/NekoGui.hpp"

#include <QPainter>

QPixmap Icon::GetTrayIcon(Icon::TrayIconStatus status) {
    QPixmap pixmap;

    // software embedded icon
    auto pixmap_read = QPixmap(":/neko/" + software_name.toLower() + ".png");
    if (!pixmap_read.isNull()) pixmap = pixmap_read;

    // software pack icon
    pixmap_read = QPixmap("../" + software_name.toLower() + ".png");
    if (!pixmap_read.isNull()) pixmap = pixmap_read;

    // user icon
    pixmap_read = QPixmap("./" + software_name.toLower() + ".png");
    if (!pixmap_read.isNull()) pixmap = pixmap_read;

    if (status == TrayIconStatus::NONE) return pixmap;

    auto p = QPainter(&pixmap);

    auto side = pixmap.width();
    auto radius = side * 0.4;
    auto d = side * 0.3;
    auto margin = side * 0.05;

    if (status == TrayIconStatus::RUNNING) {
        p.setBrush(QBrush(Qt::darkGreen));
    } else if (status == TrayIconStatus::SYSTEM_PROXY) {
        p.setBrush(QBrush(Qt::blue));
    } else if (status == TrayIconStatus::VPN) {
        p.setBrush(QBrush(Qt::red));
    }
    p.drawRoundedRect(
        QRect(side - d - margin,
              side - d - margin,
              d,
              d),
        radius,
        radius);
    p.end();

    return pixmap;
}

QPixmap Icon::GetMaterialIcon(const QString &name) {
    QPixmap pixmap(":/icon/material/" + name + ".svg");
    return pixmap;
}

QIcon Icon::GetTrayQIcon(TrayIconStatus status) {
#ifdef Q_OS_MACOS
    // macOS 菜单栏要的是"模板图"：纯黑 + alpha，系统按浅色/深色菜单栏自动反色。
    // 应用图标是 256px 的彩色插画，缩到 18pt 糊成一团，所以这里现画一个专用字形：
    // 纸箱 + 两只猫耳（和 mascot 同一个意象），小尺寸下还认得出。
    const int side = 18;
    const qreal dpr = 2.0; // 视网膜屏按 2x 出图，避免系统放大后发虚

    QPixmap pm(qRound(side * dpr), qRound(side * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);

    const qreal w = side;
    // 菜单栏图标留 8% 边距，否则和相邻状态项贴在一起
    p.translate(w * 0.08, w * 0.08);
    p.scale(0.84, 0.84);

    // 猫耳（竖在箱子后面），先画，箱体盖住底边
    p.drawPolygon(QPolygonF{
            QPointF(w * 0.20, w * 0.46), QPointF(w * 0.29, w * 0.14), QPointF(w * 0.45, w * 0.46)});
    p.drawPolygon(QPolygonF{
            QPointF(w * 0.55, w * 0.46), QPointF(w * 0.71, w * 0.14), QPointF(w * 0.80, w * 0.46)});

    // 箱体
    QRectF box(w * 0.13, w * 0.42, w * 0.74, w * 0.44);
    p.drawRoundedRect(box, w * 0.07, w * 0.07);

    // 箱盖缝：掏一条空，让他读起来是"开口的箱子"而不是一块实心方块
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillRect(QRectF(box.left() + w * 0.07, box.top() + w * 0.11,
                      box.width() - w * 0.14, w * 0.055),
               Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // 状态点：模板图没有颜色，"有没有点"就是唯一的开关指示
    if (status != TrayIconStatus::NONE) {
        p.drawEllipse(QPointF(w * 0.84, w * 0.84), w * 0.10, w * 0.10);
    }
    p.end();

    QIcon icon(pm);
    icon.setIsMask(true);
    return icon;
#else
    return QIcon(GetTrayIcon(status));
#endif
}
