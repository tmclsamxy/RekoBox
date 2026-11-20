#include "sys/macos/MacPlatform.hpp"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>

namespace NekoGui_mac {

    QString DataDir() {
        // AppDataLocation = ~/Library/Application Support/<applicationName>
        // applicationName 在 main.cpp 里被设为 "rekobox"
        auto dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dir.isEmpty()) {
            dir = QDir::homePath() + "/Library/Application Support/rekobox";
        }
        return dir;
    }

    void ApplyLook() {
        // macOS 原生 style 会把按钮压到 22px、下拉换成原生外观，和 Windows 版差一截。
        // 固定 Fusion 之后，同一份 QSS 在两边的观感才一致。
        if (auto fusion = QStyleFactory::create("Fusion")) {
            QApplication::setStyle(fusion);
        }

        // 字体不写死族名：沿用系统 UI 字体（SF Pro / PingFang SC），只把字号定到 13pt。
        // QSS 里的 feiyangqingyun 主题会带自己的 font-family，这里作为兜底。
        auto font = QApplication::font();
        font.setPointSize(13);
        QApplication::setFont(font);
    }

    QString ShellQuote(const QString &s) {
        QString out = s;
        out.replace("'", "'\\''");
        return "'" + out + "'";
    }

    QString AppleScriptString(const QString &s) {
        QString out = s;
        out.replace("\\", "\\\\");
        out.replace("\"", "\\\"");
        return out;
    }

} // namespace NekoGui_mac
