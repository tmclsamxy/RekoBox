#pragma once

#include <QString>

class ThemeManager {
public:
    QString system_style_name = "";
    QString current_theme = "0"; // int: 0:system 1+:builtin string: QStyleFactory

    void ApplyTheme(const QString &theme);

    // 强制重新应用最近一次的主题（绕过 current_theme 缓存）。
    // 用途：mac 上系统在深色/浅色之间切换后，"System" 主题必须重算一次调色板；
    // 而 current_theme 没变，ApplyTheme 会直接 return。
    void ReapplyTheme();

    // 订阅系统外观变化（只有 macOS 真的做事，其它平台空实现）。
    // 必须在 QApplication 构造之后调用。
    void WatchSystemAppearance();

private:
    // 最近一次 ApplyTheme 的入参，ReapplyTheme 靠它复原
    QString requested_theme;
};

extern ThemeManager *themeManager;