#include <QStyle>
#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QDebug>

#include "ThemeManager.hpp"

#ifdef Q_OS_MACOS
#include "sys/macos/Gui.hpp"
#endif

ThemeManager *themeManager = new ThemeManager;

extern QString ReadFileText(const QString &path);

namespace {
    // "System" 主题在 mac 深色下的调色板。
    //
    // 为什么不用 standardPalette()：那是 Fusion 风格的**浅色**默认值，
    // 和 NSApp 的 effectiveAppearance 毫无关系——这正是"跟随系统并不跟随"的根因。
    // 背景用中性灰而不是纯黑：纯黑配 13pt 中文正文对比过强，观感也更像 Windows 的
    // blacksoft 而不是 macOS。强调色用系统蓝。
    QPalette MacDarkPalette() {
        QPalette p;
        const QColor window(0x1E, 0x1E, 0x1E);   // 窗口底
        const QColor base(0x25, 0x25, 0x25);     // 输入框/表格底
        const QColor alt(0x2C, 0x2C, 0x2C);      // 交替行
        const QColor text(0xE6, 0xE6, 0xE6);
        const QColor dim(0x9A, 0x9A, 0x9A);
        const QColor button(0x32, 0x32, 0x32);
        const QColor accent(0x0A, 0x84, 0xFF);   // macOS 系统蓝

        p.setColor(QPalette::Window, window);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::AlternateBase, alt);
        p.setColor(QPalette::ToolTipBase, alt);
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Button, button);
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::BrightText, Qt::red);
        p.setColor(QPalette::Link, accent);
        p.setColor(QPalette::Highlight, accent);
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::PlaceholderText, dim);

        // 禁用态只压前景色，避免"灰底灰字"看不清
        p.setColor(QPalette::Disabled, QPalette::Text, dim);
        p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
        p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
        p.setColor(QPalette::Disabled, QPalette::Highlight, button);
        p.setColor(QPalette::Disabled, QPalette::HighlightedText, dim);
        return p;
    }
}

void ThemeManager::ApplyTheme(const QString &theme) {
    requested_theme = theme;
    auto internal = [=] {
        if (this->system_style_name.isEmpty()) {
            this->system_style_name = qApp->style()->objectName();
        }
        if (this->current_theme == theme) {
            return;
        }

        bool ok;
        auto themeId = theme.toInt(&ok);

        if (ok) {
            // System & Built-in
            QString qss;

            if (themeId != 0) {
                QString path;
                std::map<QString, QString> replace;
                switch (themeId) {
                    case 1:
                        path = ":/themes/feiyangqingyun/qss/flatgray.css";
                        replace[":/qss/"] = ":/themes/feiyangqingyun/qss/";
                        break;
                    case 2:
                        path = ":/themes/feiyangqingyun/qss/lightblue.css";
                        replace[":/qss/"] = ":/themes/feiyangqingyun/qss/";
                        break;
                    case 3:
                        path = ":/themes/feiyangqingyun/qss/blacksoft.css";
                        replace[":/qss/"] = ":/themes/feiyangqingyun/qss/";
                        break;
                    default:
                        return;
                }
                qss = ReadFileText(path);
                for (auto const &[a, b]: replace) {
                    qss = qss.replace(a, b);
                }
            }

            auto system_style = QStyleFactory::create(this->system_style_name);

            if (themeId == 0) {
                // system theme
#ifdef Q_OS_MACOS
                // "System" 必须真的跟随系统：深色时换深色调色板。
                // 原来固定 standardPalette()（Fusion 浅色），系统切深色后界面仍是亮的。
                if (NekoGui_mac::IsSystemDarkMode()) {
                    qApp->setPalette(MacDarkPalette());
                } else {
                    qApp->setPalette(system_style->standardPalette());
                }
#else
                qApp->setPalette(system_style->standardPalette());
#endif
                qApp->setStyle(system_style);
                qApp->setStyleSheet("");
            } else {
                if (themeId == 1 || themeId == 2 || themeId == 3) {
                    // feiyangqingyun theme
                    QString paletteColor = qss.mid(20, 7);
                    qApp->setPalette(QPalette(paletteColor));
                } else {
                    // other theme
                    qApp->setPalette(system_style->standardPalette());
                }
                qApp->setStyleSheet(qss);
            }
        } else {
            // QStyleFactory
            const auto &_style = QStyleFactory::create(theme);
            if (_style != nullptr) {
                qApp->setPalette(_style->standardPalette());
                qApp->setStyle(_style);
                qApp->setStyleSheet("");
            }
        }

        current_theme = theme;
    };
    internal();

    auto nekoray_css = ReadFileText(":/neko/neko.css");
    qApp->setStyleSheet(qApp->styleSheet().append("\n").append(nekoray_css));
}

void ThemeManager::ReapplyTheme() {
    // 入参为空说明还没 ApplyTheme 过，此时没有"上一个主题"可恢复
    if (requested_theme.isEmpty()) return;
    // 清掉缓存，让 internal() 真的重新算一遍（否则 current_theme == theme 会直接 return）
    current_theme.clear();
    ApplyTheme(requested_theme);
}

void ThemeManager::WatchSystemAppearance() {
#ifdef Q_OS_MACOS
    // 用户在「系统设置 → 外观」切换、或"自动"到点切换时重建调色板。
    // 只有当前是 System 主题才需要重算：固定主题（flatgray 等）与系统外观无关。
    NekoGui_mac::SetAppearanceChangeCallback([] {
        if (themeManager == nullptr) return;
        bool ok = false;
        const auto id = themeManager->requested_theme.toInt(&ok);
        if (!ok || id != 0) return;
        qDebug() << "[mac] system appearance changed, dark =" << NekoGui_mac::IsSystemDarkMode();
        themeManager->ReapplyTheme();
    });
#endif
}