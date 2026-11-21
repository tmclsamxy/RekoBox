#include "dialog_hotkey.h"
#include "ui_dialog_hotkey.h"

#include "ui/mainwindow_interface.h"

#include <QMessageBox>

DialogHotkey::DialogHotkey(QWidget *parent) : QDialog(parent), ui(new Ui::DialogHotkey) {
    ui->setupUi(this);
    ui->show_mainwindow->setKeySequence(NekoGui::dataStore->hotkey_mainwindow);
    ui->show_groups->setKeySequence(NekoGui::dataStore->hotkey_group);
    ui->show_routes->setKeySequence(NekoGui::dataStore->hotkey_route);
    ui->system_proxy->setKeySequence(NekoGui::dataStore->hotkey_system_proxy_menu);
    GetMainWindow()->RegisterHotkey(true);

    // 保存与重新注册放在 finished 里做，而不是析构：
    //  1) 析构时对话框已经不可见了，再弹模态提示会跑嵌套事件循环，容易踩到
    //     "对象正在销毁"的坑；
    //  2) 注册成不成功只有 RegisterHotkey 跑完才知道，正好在这之后提示。
    connect(this, &QDialog::finished, this, [this](int r) {
        if (r == QDialog::Accepted) {
            NekoGui::dataStore->hotkey_mainwindow = ui->show_mainwindow->keySequence().toString();
            NekoGui::dataStore->hotkey_group = ui->show_groups->keySequence().toString();
            NekoGui::dataStore->hotkey_route = ui->show_routes->keySequence().toString();
            NekoGui::dataStore->hotkey_system_proxy_menu = ui->system_proxy->keySequence().toString();
            NekoGui::dataStore->Save();
        }
        GetMainWindow()->RegisterHotkey(false);

        // 原来是静默 deleteLater：注册不上就什么都没有，用户只觉得"按了没反应"。
        // macOS 上全局热键尤其容易和系统快捷键撞车，所以这里必须说话。
        const auto failures = GetMainWindow()->HotkeyFailureSummary();
        if (r == QDialog::Accepted && !failures.isEmpty()) {
            MessageBoxWarning(software_name,
                              tr("These hotkeys could not be registered:\n\n%1\n\n"
                                 "They are most likely taken by another application or by a "
                                 "system shortcut. Choose a different combination, or free the "
                                 "conflicting one in System Settings → Keyboard → "
                                 "Keyboard Shortcuts.")
                                      .arg(failures));
        }
    });
}

DialogHotkey::~DialogHotkey() {
    delete ui;
}