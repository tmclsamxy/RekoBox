#include "./ui_mainwindow.h"
#include "mainwindow.h"

#include "fmt/Preset.hpp"
#include "db/ProfileFilter.hpp"
#include "db/ConfigBuilder.hpp"
#include "sub/GroupUpdater.hpp"
#include "sys/ExternalProcess.hpp"
#include "sys/AutoRun.hpp"

#include "ui/ThemeManager.hpp"
#include "ui/update/UpdateHelper.hpp"
#include "ui/Icon.hpp"
#include "ui/edit/dialog_edit_profile.h"
#include "ui/dialog_basic_settings.h"
#include "ui/dialog_manage_groups.h"
#include "ui/dialog_manage_routes.h"
#include "ui/dialog_vpn_settings.h"
#include "ui/dialog_hotkey.h"
#include "ui/dialog_account.h"
#include "sync/SyncEngine.hpp"

#include "3rdparty/fix_old_qt.h"
#include "3rdparty/qrcodegen.hpp"
#include "3rdparty/VT100Parser.hpp"
#include "3rdparty/qv2ray/v2/components/proxy/QvProxyConfigurator.hpp"

#ifndef NKR_NO_ZXING
#include "3rdparty/ZxingQtReader.hpp"
#endif

#ifdef Q_OS_WIN
#include "3rdparty/WinCommander.hpp"
#else
#ifdef Q_OS_LINUX
#include "sys/linux/LinuxCap.h"
#endif
#ifdef Q_OS_MACOS
#include "sys/macos/MacPlatform.hpp"
#include "sys/macos/Gui.hpp"
#endif
#include <unistd.h>
#endif

#include <QClipboard>
#include <QLabel>
#include <QTextBlock>
#include <QScrollBar>
#include <QScreen>
#include <QDesktopServices>
#include <QInputDialog>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>

void UI_InitMainWindow() {
    mainwindow = new MainWindow;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow) {
    mainwindow = this;
    core_process = nullptr; // 退出兜底会读它，先给个确定值
    MW_dialog_message = [=](const QString &a, const QString &b) {
        runOnUiThread([=] { dialog_message_impl(a, b); });
    };

    // Load Manager
    NekoGui::profileManager->LoadManager();

    // Setup misc UI
    themeManager->ApplyTheme(NekoGui::dataStore->theme);
    ui->setupUi(this);
    //
    connect(ui->menu_start, &QAction::triggered, this, [=]() { neko_start(); });
    connect(ui->menu_stop, &QAction::triggered, this, [=]() { neko_stop(); });
    connect(ui->tabWidget->tabBar(), &QTabBar::tabMoved, this, [=](int from, int to) {
        // use tabData to track tab & gid
        NekoGui::profileManager->groupsTabOrder.clear();
        for (int i = 0; i < ui->tabWidget->tabBar()->count(); i++) {
            NekoGui::profileManager->groupsTabOrder += ui->tabWidget->tabBar()->tabData(i).toInt();
        }
        NekoGui::profileManager->SaveManager();
    });
    ui->label_running->installEventFilter(this);
    ui->label_inbound->installEventFilter(this);
    ui->splitter->installEventFilter(this);
    //
    RegisterHotkey(false);
    //
    auto last_size = NekoGui::dataStore->mw_size.split("x");
    if (last_size.length() == 2) {
        auto w = last_size[0].toInt();
        auto h = last_size[1].toInt();
        if (w > 0 && h > 0) {
            resize(w, h);
        }
    }

    if (QDir("dashboard").count() == 0) {
        QDir().mkdir("dashboard");
        QFile::copy(":/neko/dashboard-notice.html", "dashboard/index.html");
    }

    // top bar
    ui->toolButton_program->setMenu(ui->menu_program);
    ui->toolButton_preferences->setMenu(ui->menu_preferences);
    ui->toolButton_server->setMenu(ui->menu_server);
    connect(ui->toolButton_update, &QToolButton::clicked, this, [=] { NekoGui_update::CheckForUpdates(this, true); });
    ui->menubar->setVisible(false);
    connect(ui->toolButton_url_test, &QToolButton::clicked, this, [=] { speedtest_current_group(1, true); });

    // Setup log UI
    ui->splitter->restoreState(DecodeB64IfValid(NekoGui::dataStore->splitter_state));
    qvLogDocument->setUndoRedoEnabled(false);
    ui->masterLogBrowser->setUndoRedoEnabled(false);
    ui->masterLogBrowser->setDocument(qvLogDocument);
    ui->masterLogBrowser->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    {
        auto font = ui->masterLogBrowser->font();
        font.setPointSize(9);
        ui->masterLogBrowser->setFont(font);
        qvLogDocument->setDefaultFont(font);
    }
    connect(ui->masterLogBrowser->verticalScrollBar(), &QSlider::valueChanged, this, [=](int value) {
        if (ui->masterLogBrowser->verticalScrollBar()->maximum() == value)
            qvLogAutoScoll = true;
        else
            qvLogAutoScoll = false;
    });
    connect(ui->masterLogBrowser, &QTextBrowser::textChanged, this, [=]() {
        if (!qvLogAutoScoll)
            return;
        auto bar = ui->masterLogBrowser->verticalScrollBar();
        bar->setValue(bar->maximum());
    });
    MW_show_log = [=](const QString &log) {
        runOnUiThread([=] { show_log_impl(log); });
    };
    MW_show_log_ext = [=](const QString &tag, const QString &log) {
        runOnUiThread([=] { show_log_impl("[" + tag + "] " + log); });
    };
    MW_show_log_ext_vt100 = [=](const QString &log) {
        runOnUiThread([=] { show_log_impl(cleanVT100String(log)); });
    };

    // table UI
    ui->proxyListTable->callback_save_order = [=] {
        auto group = NekoGui::profileManager->CurrentGroup();
        group->order = ui->proxyListTable->order;
        group->Save();
    };
    ui->proxyListTable->refresh_data = [=](int id) { refresh_proxy_list_impl_refresh_data(id); };
    if (auto button = ui->proxyListTable->findChild<QAbstractButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        // Corner Button
        connect(button, &QAbstractButton::clicked, this, [=] { refresh_proxy_list_impl(-1, {GroupSortMethod::ById}); });
    }
    connect(ui->proxyListTable->horizontalHeader(), &QHeaderView::sectionClicked, this, [=](int logicalIndex) {
        GroupSortAction action;
        // 不正确的descending实现
        if (proxy_last_order == logicalIndex) {
            action.descending = true;
            proxy_last_order = -1;
        } else {
            proxy_last_order = logicalIndex;
        }
        action.save_sort = true;
        // 表头
        if (logicalIndex == 0) {
            action.method = GroupSortMethod::ByType;
        } else if (logicalIndex == 1) {
            action.method = GroupSortMethod::ByAddress;
        } else if (logicalIndex == 2) {
            action.method = GroupSortMethod::ByName;
        } else if (logicalIndex == 3) {
            action.method = GroupSortMethod::ByLatency;
        } else {
            return;
        }
        refresh_proxy_list_impl(-1, action);
    });
    connect(ui->proxyListTable->horizontalHeader(), &QHeaderView::sectionResized, this, [=](int logicalIndex, int oldSize, int newSize) {
        auto group = NekoGui::profileManager->CurrentGroup();
        if (NekoGui::dataStore->refreshing_group || group == nullptr || !group->manually_column_width) return;
        // save manually column width
        group->column_width.clear();
        for (int i = 0; i < ui->proxyListTable->horizontalHeader()->count(); i++) {
            group->column_width.push_back(ui->proxyListTable->horizontalHeader()->sectionSize(i));
        }
        group->column_width[logicalIndex] = newSize;
        group->Save();
    });
    ui->tableWidget_conn->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableWidget_conn->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tableWidget_conn->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ui->proxyListTable->verticalHeader()->setDefaultSectionSize(24);

    // search box
    ui->search->setVisible(false);
    connect(shortcut_ctrl_f, &QShortcut::activated, this, [=] {
        ui->search->setVisible(true);
        ui->search->setFocus();
    });
    connect(shortcut_esc, &QShortcut::activated, this, [=] {
        if (ui->search->isVisible()) {
            ui->search->setText("");
            ui->search->textChanged("");
            ui->search->setVisible(false);
        }
        if (select_mode) {
            emit profile_selected(-1);
            select_mode = false;
            refresh_status();
        }
    });
    connect(ui->search, &QLineEdit::textChanged, this, [=](const QString &text) {
        if (text.isEmpty()) {
            for (int i = 0; i < ui->proxyListTable->rowCount(); i++) {
                ui->proxyListTable->setRowHidden(i, false);
            }
        } else {
            QList<QTableWidgetItem *> findItem = ui->proxyListTable->findItems(text, Qt::MatchContains);
            for (int i = 0; i < ui->proxyListTable->rowCount(); i++) {
                ui->proxyListTable->setRowHidden(i, true);
            }
            for (auto item: findItem) {
                if (item != nullptr) ui->proxyListTable->setRowHidden(item->row(), false);
            }
        }
    });

    // refresh
    this->refresh_groups();

    // Setup Tray
    tray = new QSystemTrayIcon(this); // 初始化托盘对象tray
    tray->setIcon(Icon::GetTrayQIcon(Icon::NONE));
#ifdef Q_OS_MACOS
    // macOS 用**原生**菜单栏：QMenuBar 会变成 NSMenu，而 Qt 会把挂在菜单栏上的
    // QMenu 的 platform menu 绑到 NSMenu 上。此时同一个 QMenu 再拿来当托盘右键菜单
    // （setContextMenu + popup），典型结果是托盘菜单空白 / 菜单栏那一项消失 /
    // 工具栏下拉弹不出来——三处共用一个 QMenu 在 mac 上必然打架。
    //
    // 所以 mac 上给托盘单建一个 QMenu：QAction 可以共享（同一个 action 绑多个菜单
    // 是合法的，信号只连一次），但 QMenu 本身必须是独立对象。
    // 内容也按 mac 习惯精简：完整功能在菜单栏里已经有了，托盘只留高频几项。
    tray_menu = new QMenu(this);
    tray->setContextMenu(tray_menu);
#else
    tray->setContextMenu(ui->menu_program); // 创建托盘菜单
#endif
    tray->show();                           // 让托盘图标显示在系统托盘上
#ifdef Q_OS_MACOS
    // 菜单栏项是 mac 上最容易"静默失败"的一环（图标不显示、点了没反应都不报错），
    // 启动时把状态打出来，出问题时 `open -W` 或终端里跑就能看到
    qDebug() << "[mac] tray available:" << QSystemTrayIcon::isSystemTrayAvailable()
             << "visible:" << tray->isVisible()
             << "supportsMessages:" << QSystemTrayIcon::supportsMessages();
#endif
    connect(tray, &QSystemTrayIcon::activated, this, [=](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            if (this->isVisible()) {
                hide();
            } else {
                ActivateWindow(this);
            }
        }
    });

    // Misc menu
    connect(ui->menu_open_config_folder, &QAction::triggered, this, [=] { QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::currentPath())); });
    ui->menu_program_preference->addActions(ui->menu_preferences->actions());
    connect(ui->menu_add_from_clipboard2, &QAction::triggered, ui->menu_add_from_clipboard, &QAction::trigger);
    connect(ui->actionRestart_Proxy, &QAction::triggered, this, [=] { if (NekoGui::dataStore->started_id>=0) neko_start(NekoGui::dataStore->started_id); });
    connect(ui->actionRestart_Program, &QAction::triggered, this, [=] { MW_dialog_message("", "RestartProgram"); });
    connect(ui->actionCheck_Update, &QAction::triggered, this, [=] { NekoGui_update::CheckForUpdates(this, true); });
    connect(ui->actionShow_window, &QAction::triggered, this, [=] { tray->activated(QSystemTrayIcon::ActivationReason::Trigger); });
    //
    connect(ui->menu_program, &QMenu::aboutToShow, this, [=]() {
        ui->actionRemember_last_proxy->setChecked(NekoGui::dataStore->remember_enable);
        ui->actionStart_with_system->setChecked(AutoRun_IsEnabled());
        ui->actionAllow_LAN->setChecked(QStringList{"::", "0.0.0.0"}.contains(NekoGui::dataStore->inbound_address));
        // active server
        for (const auto &old: ui->menuActive_Server->actions()) {
            ui->menuActive_Server->removeAction(old);
            old->deleteLater();
        }
        int active_server_item_count = 0;
        for (const auto &pf: NekoGui::profileManager->CurrentGroup()->ProfilesWithOrder()) {
            auto a = new QAction(pf->bean->DisplayTypeAndName(), this);
            a->setProperty("id", pf->id);
            a->setCheckable(true);
            if (NekoGui::dataStore->started_id == pf->id) a->setChecked(true);
            ui->menuActive_Server->addAction(a);
            if (++active_server_item_count == 100) break;
        }
        // active routing
        for (const auto &old: ui->menuActive_Routing->actions()) {
            ui->menuActive_Routing->removeAction(old);
            old->deleteLater();
        }
        for (const auto &name: NekoGui::Routing::List()) {
            auto a = new QAction(name, this);
            a->setCheckable(true);
            a->setChecked(name == NekoGui::dataStore->active_routing);
            ui->menuActive_Routing->addAction(a);
        }
    });
    #ifdef Q_OS_MACOS
    // 托盘菜单：只放高频几项，完整菜单在菜单栏里。
    // QAction 是共享的（同一个 action 挂到多个菜单是 Qt 支持的用法，信号也只连一次），
    // 但 QMenu 必须是独立对象——这正是这条 bug 的根因。
    tray_menu->addAction(ui->actionShow_window);
    tray_menu->addSeparator();
    // 系统代理/路由这类子菜单也必须是新 QMenu：直接把 ui->menu_spmode 塞进来
    // 就又会和菜单栏抢同一个 platform menu。
    auto tray_spmode = tray_menu->addMenu(tr("系统代理"));
    tray_spmode->addAction(ui->menu_spmode_system_proxy);
    tray_spmode->addAction(ui->menu_spmode_vpn);
    tray_spmode->addAction(ui->menu_spmode_disabled);
    auto tray_server = tray_menu->addMenu(tr("当前服务器"));
    tray_menu->addSeparator();
    tray_menu->addAction(ui->actionRestart_Proxy);
    tray_menu->addAction(ui->menu_exit);

    // 勾选状态每次弹出前同步一次（与 menu_program 的 aboutToShow 同一份逻辑，
    // 不能共用同一个 QMenu 的 aboutToShow，所以这里单独接一个）
    connect(tray_menu, &QMenu::aboutToShow, this, [=]() {
        ui->menu_spmode_disabled->setChecked(!(NekoGui::dataStore->spmode_system_proxy || NekoGui::dataStore->spmode_vpn));
        ui->menu_spmode_system_proxy->setChecked(NekoGui::dataStore->spmode_system_proxy);
        ui->menu_spmode_vpn->setChecked(NekoGui::dataStore->spmode_vpn);

        // 当前服务器列表：与菜单栏不同，这里只列前 20 个，避免托盘菜单撑满屏幕
        for (const auto &old: tray_server->actions()) {
            tray_server->removeAction(old);
            delete old; // 这些是托盘专用的临时 action，不共享
        }
        int count = 0;
        for (const auto &pf: NekoGui::profileManager->CurrentGroup()->ProfilesWithOrder()) {
            auto a = new QAction(pf->bean->DisplayTypeAndName(), this);
            a->setProperty("id", pf->id);
            a->setCheckable(true);
            if (NekoGui::dataStore->started_id == pf->id) a->setChecked(true);
            connect(a, &QAction::triggered, this, [=] {
                auto id = a->property("id").toInt();
                if (NekoGui::dataStore->started_id == id) {
                    neko_stop();
                } else {
                    neko_start(id);
                }
            });
            tray_server->addAction(a);
            if (++count == 20) {
                // 这里**不能** setMenu(ui->menuActive_Server)：那等于把菜单栏里的
                // QMenu 再借给托盘，正好是本条 bug 本身（一个 QMenu 被 NSMenu 绑过
                // 之后不能再当 popup 用）。要"更多"就把窗口打开，让用户用完整菜单。
                tray_server->addSeparator();
                auto more = tray_server->addAction(tr("More…"));
                connect(more, &QAction::triggered, this, [=] { ActivateWindow(this); });
                break;
            }
        }
        if (count == 0) {
            auto a = tray_server->addAction(tr("(no profiles)"));
            a->setEnabled(false);
        }
    });

    // 自检日志：托盘菜单在 mac 上"空白"是这条 bug 的典型表现，而没有屏幕录制权限时
    // 截图看不到菜单栏，所以把结构打进日志——出问题时看日志就能判断菜单是否建起来了。
    connect(tray, &QSystemTrayIcon::activated, this, [=](QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Context) return;
        qDebug() << "[mac] tray context menu opened, top-level items:"
                 << tray_menu->actions().size()
                 << "targets a separate QMenu =" << (tray_menu != ui->menu_program)
                 << "menu_program still parented to menubar ="
                 << (ui->menu_program->parentWidget() == ui->menubar);
    });
#endif
    connect(ui->menuActive_Server, &QMenu::triggered, this, [=](QAction *a) {
        bool ok;
        auto id = a->property("id").toInt(&ok);
        if (!ok) return;
        if (NekoGui::dataStore->started_id == id) {
            neko_stop();
        } else {
            neko_start(id);
        }
    });
    connect(ui->menuActive_Routing, &QMenu::triggered, this, [=](QAction *a) {
        auto fn = a->text();
        if (!fn.isEmpty()) {
            NekoGui::Routing r;
            r.load_control_must = true;
            r.fn = ROUTES_PREFIX + fn;
            if (r.Load()) {
                if (QMessageBox::question(GetMessageBoxParent(), software_name, tr("Load routing and apply: %1").arg(fn) + "\n" + r.DisplayRouting()) == QMessageBox::Yes) {
                    NekoGui::Routing::SetToActive(fn);
                    if (NekoGui::dataStore->started_id >= 0) {
                        neko_start(NekoGui::dataStore->started_id);
                    } else {
                        refresh_status();
                    }
                }
            }
        }
    });
    connect(ui->actionRemember_last_proxy, &QAction::triggered, this, [=](bool checked) {
        NekoGui::dataStore->remember_enable = checked;
        NekoGui::dataStore->Save();
    });
    connect(ui->actionStart_with_system, &QAction::triggered, this, [=](bool checked) {
        // macOS 上这个勾选框有两个容易踩的坑，都在这里显式处理，
        // 别让用户看到"勾了又跳回去"却没人解释：
        //   1) App 不在 /Applications → SMAppService 恒返回 NotFound，注册不可能成功
        //   2) 注册成功但需用户在「系统设置 → 通用 → 登录项」批准 → 状态是 RequiresApproval
        if (!AutoRun_SetEnabled(checked)) {
            // 复位勾选框：菜单的 aboutToShow 里会用真实状态覆盖，但菜单此时已经关掉了，
            // 不手动复位的话界面会一直显示"已开启"直到下次打开菜单。
            ui->actionStart_with_system->setChecked(AutoRun_IsEnabled());
#ifdef Q_OS_MACOS
            MessageBoxWarning(
                software_name,
                tr("Could not change the \"Start at login\" setting.\n\n"
                   "On macOS this requires RekoBox to be in the Applications folder:\n"
                   "drag RekoBox.app to /Applications, launch it from there, then try again."));
#endif
            return;
        }
#ifdef Q_OS_MACOS
        if (checked && AutoRun_Status() == 2) {
            QMessageBox::information(
                GetMessageBoxParent(), software_name,
                tr("One more step needed.\n\n"
                   "RekoBox has been added to login items, but macOS needs your approval:\n"
                   "System Settings → General → Login Items → allow RekoBox.\n\n"
                   "Also note: login items cannot pass command-line arguments, so RekoBox "
                   "will start hidden in the menu bar (same as the Windows \"-tray\" behaviour)."));
        }
#endif
    });
    connect(ui->actionAllow_LAN, &QAction::triggered, this, [=](bool checked) {
        NekoGui::dataStore->inbound_address = checked ? "::" : "127.0.0.1";
        MW_dialog_message("", "UpdateDataStore");
    });
    //
    connect(ui->checkBox_VPN, &QCheckBox::clicked, this, [=](bool checked) { neko_set_spmode_vpn(checked); });
    connect(ui->checkBox_SystemProxy, &QCheckBox::clicked, this, [=](bool checked) { neko_set_spmode_system_proxy(checked); });
    connect(ui->menu_spmode, &QMenu::aboutToShow, this, [=]() {
        ui->menu_spmode_disabled->setChecked(!(NekoGui::dataStore->spmode_system_proxy || NekoGui::dataStore->spmode_vpn));
        ui->menu_spmode_system_proxy->setChecked(NekoGui::dataStore->spmode_system_proxy);
        ui->menu_spmode_vpn->setChecked(NekoGui::dataStore->spmode_vpn);
    });
    connect(ui->menu_spmode_system_proxy, &QAction::triggered, this, [=](bool checked) { neko_set_spmode_system_proxy(checked); });
    connect(ui->menu_spmode_vpn, &QAction::triggered, this, [=](bool checked) { neko_set_spmode_vpn(checked); });
    connect(ui->menu_spmode_disabled, &QAction::triggered, this, [=]() {
        neko_set_spmode_system_proxy(false);
        neko_set_spmode_vpn(false);
    });
    connect(ui->menu_qr, &QAction::triggered, this, [=]() { display_qr_link(false); });
    connect(ui->menu_tcp_ping, &QAction::triggered, this, [=]() { speedtest_current_group(0, false); });
    connect(ui->menu_url_test, &QAction::triggered, this, [=]() { speedtest_current_group(1, false); });
    connect(ui->menu_full_test, &QAction::triggered, this, [=]() { speedtest_current_group(2, false); });
    connect(ui->menu_stop_testing, &QAction::triggered, this, [=]() { speedtest_current_group(114514, false); });
    //
    auto set_selected_or_group = [=](int mode) {
        // 0=group 1=select 2=unknown(menu is hide)
        ui->menu_server->setProperty("selected_or_group", mode);
    };
    auto move_tests_to_menu = [=](bool menuCurrent_Select) {
        return [=] {
            if (menuCurrent_Select) {
                ui->menuCurrent_Select->insertAction(ui->actionfake_4, ui->menu_tcp_ping);
                ui->menuCurrent_Select->insertAction(ui->actionfake_4, ui->menu_url_test);
                ui->menuCurrent_Select->insertAction(ui->actionfake_4, ui->menu_full_test);
                ui->menuCurrent_Select->insertAction(ui->actionfake_4, ui->menu_stop_testing);
                ui->menuCurrent_Select->insertAction(ui->actionfake_4, ui->menu_clear_test_result);
                ui->menuCurrent_Select->insertAction(ui->actionfake_4, ui->menu_resolve_domain);
            } else {
                ui->menuCurrent_Group->insertAction(ui->actionfake_5, ui->menu_tcp_ping);
                ui->menuCurrent_Group->insertAction(ui->actionfake_5, ui->menu_url_test);
                ui->menuCurrent_Group->insertAction(ui->actionfake_5, ui->menu_full_test);
                ui->menuCurrent_Group->insertAction(ui->actionfake_5, ui->menu_stop_testing);
                ui->menuCurrent_Group->insertAction(ui->actionfake_5, ui->menu_clear_test_result);
                ui->menuCurrent_Group->insertAction(ui->actionfake_5, ui->menu_resolve_domain);
            }
            set_selected_or_group(menuCurrent_Select ? 1 : 0);
        };
    };
    connect(ui->menuCurrent_Select, &QMenu::aboutToShow, this, move_tests_to_menu(true));
    connect(ui->menuCurrent_Group, &QMenu::aboutToShow, this, move_tests_to_menu(false));
    connect(ui->menu_server, &QMenu::aboutToHide, this, [=] {
        setTimeout([=] { set_selected_or_group(2); }, this, 200);
    });
    set_selected_or_group(2);
    //
    connect(ui->menu_share_item, &QMenu::aboutToShow, this, [=] {
        QString name;
        auto selected = get_now_selected_list();
        if (!selected.isEmpty()) {
            auto ent = selected.first();
            name = ent->bean->DisplayCoreType();
        }
        ui->menu_export_config->setVisible(name == software_core_name);
        ui->menu_export_config->setText(tr("Export %1 config").arg(name));
    });
    refresh_status();

    // Prepare core
    NekoGui::dataStore->core_token = GetRandomString(32);
    NekoGui::dataStore->core_port = MkPort();
    if (NekoGui::dataStore->core_port <= 0) NekoGui::dataStore->core_port = 19810;

    // 统一走 FindNekoBoxCoreRealPath()，它会按平台补 .exe
    auto core_path = NekoGui::FindNekoBoxCoreRealPath();

    QStringList args;
    args.push_back("nekobox");
    args.push_back("-port");
    args.push_back(Int2String(NekoGui::dataStore->core_port));
    if (NekoGui::dataStore->flag_debug) args.push_back("-debug");

    // Start core
    // 注意：原来这里传了 DS_cores，注释写的却是 "main thread"——实际回调跑在 cores
    // 线程上，core_process（QProcess）就在那个线程被创建并 start()，而 UI 线程的
    // processId()/state()/kill()/Restart() 全部跨线程直接调用该 QProcess 的内部状态。
    // QProcess 的通道 notifier 在两边同时被碰就是 2026-11-20 那批
    // `~QObject` / `~ConnectionData` 重复释放崩溃的典型成因（P0-2）。
    // 内核进程走 QProcess 自己的线程即可，创建句柄留在 UI 线程，所有成员访问
    // （processId/state/kill）都在同一个线程，才谈得上线程安全。
    core_process = new NekoGui_sys::CoreProcess(core_path, args);
    // Remember last started
    if (NekoGui::dataStore->remember_enable && NekoGui::dataStore->remember_id >= 0) {
        core_process->start_profile_when_core_is_up = NekoGui::dataStore->remember_id;
    }
    // Setup
    core_process->Start();
    setup_grpc();

    // Remember system proxy
    if (NekoGui::dataStore->remember_enable || NekoGui::dataStore->flag_restart_tun_on) {
        if (NekoGui::dataStore->remember_spmode.contains("system_proxy")) {
            neko_set_spmode_system_proxy(true, false);
        }
        if (NekoGui::dataStore->remember_spmode.contains("vpn") || NekoGui::dataStore->flag_restart_tun_on) {
            neko_set_spmode_vpn(true, false);
        }
    }

    connect(qApp, &QGuiApplication::commitDataRequest, this, &MainWindow::on_commitDataRequest);

    auto t = new QTimer;
    connect(t, &QTimer::timeout, this, [=]() { refresh_status(); });
    t->start(2000);

    t = new QTimer;
    connect(t, &QTimer::timeout, this, [&] { NekoGui_sys::logCounter.fetchAndStoreRelaxed(0); });
    t->start(1000);

    TM_auto_update_subsctiption = new QTimer;
    TM_auto_update_subsctiption_Reset_Minute = [&](int m) {
        TM_auto_update_subsctiption->stop();
        if (m >= 30) TM_auto_update_subsctiption->start(m * 60 * 1000);
    };
    connect(TM_auto_update_subsctiption, &QTimer::timeout, this, [&] { UI_update_all_groups(true); });
    TM_auto_update_subsctiption_Reset_Minute(NekoGui::dataStore->sub_auto_update);

    // 云端同步：登录状态下启动 3 秒后拉一次，之后本地改动自动推送
    NekoGui_sync::SyncEngine::instance()->init();

#ifdef Q_OS_MACOS
    // 托盘模式下把 Dock 图标也收掉，否则会同时有"菜单栏图标 + Dock 图标 + Cmd+Tab 项"，
    // 而 Windows 上 -tray 是纯托盘（无任务栏按钮）。这个差异只能运行时改
    // ActivationPolicy 做到（Info.plist 的 LSUIElement 是编译期固定的，
    // 改了就没法再切回普通窗口模式）。
    if (NekoGui::dataStore->flag_tray) {
        NekoGui_mac::SetDockIconVisible(false);
    }

    // P2-16：App Nap 抑制。
    // 代理软件"挂后台"是常态，而窗口不可见一段时间后系统会合并/节流定时器，
    // TrafficLooper 靠 msleep 轮询，最先中招——表现就是"挂久了流量统计不跳"。
    // 用 NSActivityUserInitiatedAllowingIdleSystemSleep：只挡 App Nap，不挡系统睡眠
    // （笔记本上阻止休眠很扰人，不该由代理软件决定）。
    NekoGui_mac::SetAppNapInhibited(true);

    // P2-16：睡眠唤醒 / 网络变化。
    //   唤醒后 utun 与路由可能已被系统拆掉，界面仍显示"TUN 已开启"但实际不通；
    //   换 Wi-Fi / 插网线后 route_exclude_address 里的节点 IP 可能来自旧 DNS。
    // 两者都只提示 + 让用户一键重启代理，不自动重连：自动重连会在
    // "用户刻意断开"的情况下造成"自己又连上了"的困惑。
    NekoGui_mac::SetPowerEventCallbacks(
            [] {
                qInfo() << "[mac] system woke up";
                runOnUiThread([] {
                    MW_show_log("[mac] 系统已唤醒。若无法上网，请重启代理（TUN 的 utun 与路由可能已被系统回收）。");
                });
            },
            [] {
                qInfo() << "[mac] network configuration changed";
                runOnUiThread([] {
                    MW_show_log("[mac] 网络已切换。若节点连不上，请重启代理（节点地址可能已变）。");
                });
            });
#endif

    if (!NekoGui::dataStore->flag_tray) show();

    // 兜底：任何退出路径（包括直接 qApp->quit()）都要确保内核进程结束，
    // 否则它会占着 rekobox_core.exe 和本地端口，下次启动报 bind 失败
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        NekoGui::dataStore->prepare_exit = true;
        if (core_process != nullptr && core_process->state() != QProcess::NotRunning) {
            // 这里只能 kill，**不能** waitForFinished：aboutToQuit 触发时事件循环已在退出，
            // waitForFinished 会跑嵌套循环，QProcess 通道的 QSocketNotifier 在析构阶段
            // 踩空 → SIGSEGV（2026-11-20 实测崩溃报告：aboutToQuit → waitForFinished →
            // ~QSocketNotifier）。kill 是 SIGKILL，进程一样会立刻死，端口也会释放。
            core_process->kill();
        }
    });

    // 上次更新若没替换成功（脚本会留日志），先告诉用户，别让他以为已经升级
    QTimer::singleShot(1500, this, [this] { NekoGui_update::CheckLastUpdateResult(this); });

    // 启动后静默检查更新：只在新版本存在时弹窗，失败不打扰
    QTimer::singleShot(8000, this, [this] { NekoGui_update::CheckForUpdates(this, false); });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (tray->isVisible()) {
        hide();          // 隐藏窗口
#ifdef Q_OS_MACOS
        // 托盘模式下窗口收起来就恢复"纯菜单栏"，Dock 图标跟着消失——
        // 与 Windows 上 -tray 的"关掉窗口只剩托盘"一致。
        if (NekoGui::dataStore->flag_tray) {
            NekoGui_mac::SetDockIconVisible(false);
        }
#endif
        event->ignore(); // 忽略事件
    }
}

#ifdef Q_OS_MACOS
void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);

    // 托盘模式启动时是 Accessory（无 Dock 图标）。窗口一露头就要切回 Regular：
    // Accessory 应用的窗口不参与正常的窗口层次，用户会觉得"窗口跑到别的 App 后面去了"。
    // 系统对"已经是 Regular"再设一次是幂等的，所以这里不必自己记状态。
    if (NekoGui::dataStore->flag_tray) {
        NekoGui_mac::SetDockIconVisible(true);
    }
}
#endif

MainWindow::~MainWindow() {
    delete ui;
}

// Group tab manage

inline int tabIndex2GroupId(int index) {
    if (NekoGui::profileManager->groupsTabOrder.length() <= index) return -1;
    return NekoGui::profileManager->groupsTabOrder[index];
}

inline int groupId2TabIndex(int gid) {
    for (int key = 0; key < NekoGui::profileManager->groupsTabOrder.count(); key++) {
        if (NekoGui::profileManager->groupsTabOrder[key] == gid) return key;
    }
    return 0;
}

void MainWindow::on_tabWidget_currentChanged(int index) {
    if (NekoGui::dataStore->refreshing_group_list) return;
    if (tabIndex2GroupId(index) == NekoGui::dataStore->current_group) return;
    show_group(tabIndex2GroupId(index));
}

void MainWindow::show_group(int gid) {
    if (NekoGui::dataStore->refreshing_group) return;
    NekoGui::dataStore->refreshing_group = true;

    auto group = NekoGui::profileManager->GetGroup(gid);
    if (group == nullptr) {
        MessageBoxWarning(tr("Error"), QStringLiteral("No such group: %1").arg(gid));
        NekoGui::dataStore->refreshing_group = false;
        return;
    }

    if (NekoGui::dataStore->current_group != gid) {
        NekoGui::dataStore->current_group = gid;
        NekoGui::dataStore->Save();
    }
    ui->tabWidget->widget(groupId2TabIndex(gid))->layout()->addWidget(ui->proxyListTable);

    // 列宽是否可调
    if (group->manually_column_width) {
        for (int i = 0; i <= 4; i++) {
            ui->proxyListTable->horizontalHeader()->setSectionResizeMode(i, QHeaderView::Interactive);
            auto size = group->column_width.value(i);
            if (size <= 0) size = ui->proxyListTable->horizontalHeader()->defaultSectionSize();
            ui->proxyListTable->horizontalHeader()->resizeSection(i, size);
        }
    } else {
        ui->proxyListTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        ui->proxyListTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        ui->proxyListTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        ui->proxyListTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        ui->proxyListTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    }

    // show proxies
    GroupSortAction gsa;
    gsa.scroll_to_started = true;
    refresh_proxy_list_impl(-1, gsa);

    NekoGui::dataStore->refreshing_group = false;
}

// callback

void MainWindow::dialog_message_impl(const QString &sender, const QString &info) {
    // info
    if (info.contains("UpdateIcon")) {
        icon_status = -1;
        refresh_status();
    }
    if (info.contains("UpdateDataStore")) {
        auto suggestRestartProxy = NekoGui::dataStore->Save();
        if (info.contains("RouteChanged")) {
            suggestRestartProxy = true;
        }
        if (info.contains("NeedRestart")) {
            suggestRestartProxy = false;
        }
        refresh_proxy_list();
        if (info.contains("VPNChanged") && NekoGui::dataStore->spmode_vpn) {
            MessageBoxWarning(tr("Tun Settings changed"), tr("Restart Tun to take effect."));
        }
        if (suggestRestartProxy && NekoGui::dataStore->started_id >= 0 &&
            QMessageBox::question(GetMessageBoxParent(), tr("Confirmation"), tr("Settings changed, restart proxy?")) == QMessageBox::StandardButton::Yes) {
            neko_start(NekoGui::dataStore->started_id);
        }
        refresh_status();
    }
    if (info.contains("NeedRestart")) {
        auto n = QMessageBox::warning(GetMessageBoxParent(), tr("Settings changed"), tr("Restart the program to take effect."), QMessageBox::Yes | QMessageBox::No);
        if (n == QMessageBox::Yes) {
            this->exit_reason = 2;
            on_menu_exit_triggered();
        }
    }
    //
    if (info == "RestartProgram") {
        this->exit_reason = 2;
        on_menu_exit_triggered();
    } else if (info == "Raise") {
        ActivateWindow(this);
    } else if (info == "ClearConnectionList") {
        refresh_connection_list({});
    }
    // sender
    if (sender == Dialog_DialogEditProfile) {
        auto msg = info.split(",");
        if (msg.contains("accept")) {
            refresh_proxy_list();
            if (msg.contains("restart")) {
                if (QMessageBox::question(GetMessageBoxParent(), tr("Confirmation"), tr("Settings changed, restart proxy?")) == QMessageBox::StandardButton::Yes) {
                    neko_start(NekoGui::dataStore->started_id);
                }
            }
        }
    } else if (sender == Dialog_DialogManageGroups) {
        if (info.startsWith("refresh")) {
            this->refresh_groups();
        }
    } else if (sender == "SyncEngine") {
        // 云端数据合并进本地：分组可能新增/重命名，节点可能增删
        if (info.startsWith("refresh")) {
            this->refresh_groups();
            this->refresh_proxy_list();
        }
    } else if (sender == "SubUpdater") {
        if (info.startsWith("finish")) {
            refresh_proxy_list();
            if (!info.contains("dingyue")) {
                show_log_impl(tr("Imported %1 profile(s)").arg(NekoGui::dataStore->imported_count));
            }
        } else if (info == "NewGroup") {
            refresh_groups();
        }
    } else if (sender == "ExternalProcess") {
        if (info == "Crashed") {
            neko_stop();
        } else if (info == "CoreCrashed") {
            neko_stop(true);
        } else if (info.startsWith("CoreStarted")) {
            neko_start(info.split(",")[1].toInt());
        }
    }
}

// top bar & tray menu

inline bool dialog_is_using = false;

#define USE_DIALOG(a)                               \
    if (dialog_is_using) return;                    \
    dialog_is_using = true;                         \
    auto dialog = new a(this);                      \
    connect(dialog, &QDialog::finished, this, [=] { \
        dialog->deleteLater();                      \
        dialog_is_using = false;                    \
    });                                             \
    dialog->show();

void MainWindow::on_menu_basic_settings_triggered() {
    USE_DIALOG(DialogBasicSettings)
}

void MainWindow::on_menu_manage_groups_triggered() {
    USE_DIALOG(DialogManageGroups)
}

void MainWindow::on_menu_routing_settings_triggered() {
    USE_DIALOG(DialogManageRoutes)
}

void MainWindow::on_menu_vpn_settings_triggered() {
    USE_DIALOG(DialogVPNSettings)
}

void MainWindow::on_menu_hotkey_settings_triggered() {
    USE_DIALOG(DialogHotkey)
}

void MainWindow::on_menu_account_triggered() {
    USE_DIALOG(DialogAccount)
}

void MainWindow::on_commitDataRequest() {
    qDebug() << "Start of data save";
    //
    if (!isMaximized()) {
        auto olds = NekoGui::dataStore->mw_size;
        auto news = QStringLiteral("%1x%2").arg(size().width()).arg(size().height());
        if (olds != news) {
            NekoGui::dataStore->mw_size = news;
        }
    }
    //
    NekoGui::dataStore->splitter_state = ui->splitter->saveState().toBase64();
    //
    auto last_id = NekoGui::dataStore->started_id;
    if (NekoGui::dataStore->remember_enable && last_id >= 0) {
        NekoGui::dataStore->remember_id = last_id;
    }
    //
    NekoGui::dataStore->Save();
    NekoGui::profileManager->SaveManager();
    // 退出前把同步状态（令牌、pending、指纹）落盘，下次启动继续补推
    NekoGui_sync::SyncEngine::instance()->persistState();
    qDebug() << "End of data save";
}

void MainWindow::on_menu_exit_triggered() {
    if (mu_exit.tryLock()) {
        NekoGui::dataStore->prepare_exit = true;
        //
        neko_set_spmode_system_proxy(false, false);
        // sync=true：退出链路必须同步把 TUN 停干净，紧接着要读 spmode_vpn 判断
        // 能不能继续退出（异步的话 spmode_vpn 还是 true，会被误判成"没停掉"）。
        // 停不掉时的兜底在 neko_set_spmode_vpn 的 sync 分支里：告警但不阻塞退出。
        neko_set_spmode_vpn(false, false, true);
        if (NekoGui::dataStore->spmode_vpn) {
            mu_exit.unlock(); // retry
            return;
        }
        RegisterHotkey(true);
        //
        on_commitDataRequest();
        //
        NekoGui::dataStore->save_control_no_save = true; // don't change datastore after this line
        neko_stop(false, true);
        //
        hide();
        runOnNewThread([=] {
            // 加超时：TUN 模式下关闭 tun 入站可能卡住 gRPC Stop，
            // 没有超时的话 sem 永不释放，后台线程挂死，GUI 与内核都留在后台
            if (!sem_stopped.tryAcquire(1, 15000)) {
                MW_show_log("[Error] stop timeout, forcing exit");
            }
            stop_core_daemon();
            runOnUiThread([=] {
                on_menu_exit_triggered(); // continue exit progress
            });
        });
        return;
    }
    //
    MF_release_runguard();

    // 兜底：无论 gRPC Exit 是否成功，退出前直接结束内核进程。
    // prepare_exit 已置位，CoreProcess 不会把它再拉起来。
    //
    // 这里**只能 kill，不能 waitForFinished**：本函数末尾就是 QCoreApplication::quit()，
    // waitForFinished 会在退出链路上跑嵌套事件循环，QProcess 通道的 QSocketNotifier
    // 在 quit 之后析构踩空 → SIGSEGV（2026-11-20 崩溃报告：
    // aboutToQuit → waitForFinished → QProcessPrivate::closeChannel → ~QSocketNotifier）。
    // 原先是 kill 之后再等 3 秒，等于把同一处崩溃往前挪了一步。kill 是 SIGKILL，
    // 进程立刻消失、端口立刻释放，不需要等。
    NekoGui::dataStore->prepare_exit = true;
    if (core_process != nullptr && core_process->state() != QProcess::NotRunning) {
        core_process->kill();
    }
    // exit_reason == 1 是"更新完要重启"（Windows：updater.exe；Linux：launcher 软链）。
    // mac 上**故意没有** updater（main.cpp:112 不建软链，build_macos.sh 还会 rm -f），
    // 原来这里 startDetached("./updater") 必然失败且不报错，用户看到的是"更新完 App 直接消失"。
    // mac 的更新流程是「打开 dmg → 用户自己拖进 Applications → 再手动启动」，
    // 所以这条分支在 mac 上什么都不做，仅记录日志。
    if (exit_reason == 1) {
#ifdef Q_OS_MACOS
        qDebug() << "[mac] exit_reason=1 (post-update restart): no updater on macOS, "
                    "the dmg installer handles the relaunch";
#else
        QDir::setCurrent(QApplication::applicationDirPath());
        QProcess::startDetached("./updater", QStringList{});
#endif
    } else if (exit_reason == 2 || exit_reason == 3) {
        QDir::setCurrent(QApplication::applicationDirPath());

        auto arguments = NekoGui::dataStore->argv;
        if (arguments.length() > 0) {
            arguments.removeFirst();
            arguments.removeAll("-tray");
            arguments.removeAll("-flag_restart_tun_on");
            arguments.removeAll("-flag_reorder");
        }
        auto isLauncher = qEnvironmentVariable("NKR_FROM_LAUNCHER") == "1";
        if (isLauncher) arguments.prepend("--");
        auto program = isLauncher ? "./launcher" : QApplication::applicationFilePath();

        if (exit_reason == 3) {
            // Tun restart as admin
            arguments << "-flag_restart_tun_on";
#ifdef Q_OS_WIN
            WinCommander::runProcessElevated(program, arguments, "", WinCommander::SW_NORMAL, false);
#else
            QProcess::startDetached(program, arguments);
#endif
        } else {
            QProcess::startDetached(program, arguments);
        }
    }
    tray->hide();
    QCoreApplication::quit();
}

#define neko_set_spmode_FAILED \
    refresh_status();          \
    return;

void MainWindow::neko_set_spmode_system_proxy(bool enable, bool save) {
    if (enable != NekoGui::dataStore->spmode_system_proxy) {
        if (enable) {
            auto socks_port = NekoGui::dataStore->inbound_socks_port;
            auto http_port = NekoGui::dataStore->inbound_socks_port;
            SetSystemProxy(http_port, socks_port);
        } else {
            ClearSystemProxy();
        }
    }

    if (save) {
        NekoGui::dataStore->remember_spmode.removeAll("system_proxy");
        if (enable && NekoGui::dataStore->remember_enable) {
            NekoGui::dataStore->remember_spmode.append("system_proxy");
        }
        NekoGui::dataStore->Save();
    }

    NekoGui::dataStore->spmode_system_proxy = enable;
    refresh_status();
}

void MainWindow::neko_set_spmode_vpn(bool enable, bool save, bool sync) {
    // 收尾：把新状态落到 dataStore 并刷界面。异步分支拿到结果后也走这里，
    // 保证"保存 remember_spmode / 刷新勾选状态"只有一份实现。
    auto apply_state = [=](bool state) {
        if (save) {
            NekoGui::dataStore->remember_spmode.removeAll("vpn");
            if (state && NekoGui::dataStore->remember_enable) {
                NekoGui::dataStore->remember_spmode.append("vpn");
            }
            NekoGui::dataStore->Save();
        }

        NekoGui::dataStore->spmode_vpn = state;
        refresh_status();

        if (NekoGui::dataStore->vpn_internal_tun && NekoGui::dataStore->started_id >= 0) neko_start(NekoGui::dataStore->started_id);
    };

    if (enable != NekoGui::dataStore->spmode_vpn) {
        if (enable) {
            if (NekoGui::dataStore->vpn_internal_tun) {
                bool requestPermission = !NekoGui::IsAdmin();
#ifdef Q_OS_MACOS
                // macOS 上进程永远不是 root，提权是在 StartVPNProcess() 里用
                // osascript "with administrator privileges" 做的（会弹系统密码框）。
                // 这里如果要求预先拿到 root，就会落到下面那个 neko_set_spmode_FAILED——
                // 因为 Linux/Win 的提权分支都不参与 mac 编译，表现就是"勾选后自动弹回、不弹框"。
                requestPermission = false;
#endif
                if (requestPermission) {
#ifdef Q_OS_LINUX
                    if (!Linux_HavePkexec()) {
                        MessageBoxWarning(software_name, "Please install \"pkexec\" first.");
                        neko_set_spmode_FAILED
                    }
                    auto ret = Linux_Pkexec_SetCapString(NekoGui::FindNekoBoxCoreRealPath(), "cap_net_admin=ep");
                    if (ret == 0) {
                        this->exit_reason = 3;
                        on_menu_exit_triggered();
                    } else {
                        MessageBoxWarning(software_name, "Setcap for Tun mode failed.\n\n1. You may canceled the dialog.\n2. You may be using an incompatible environment like AppImage.");
                        if (QProcessEnvironment::systemEnvironment().contains("APPIMAGE")) {
                            MW_show_log("If you are using AppImage, it's impossible to start a Tun. Please use other package instead.");
                        }
                    }
#endif
#ifdef Q_OS_WIN
                    auto n = QMessageBox::warning(GetMessageBoxParent(), software_name, tr("Please run NekoBox as admin"), QMessageBox::Yes | QMessageBox::No);
                    if (n == QMessageBox::Yes) {
                        this->exit_reason = 3;
                        on_menu_exit_triggered();
                    }
#endif
                    neko_set_spmode_FAILED
                }
            } else {
                if (NekoGui::dataStore->need_keep_vpn_off) {
                    MessageBoxWarning(software_name, tr("Current server is incompatible with Tun. Please stop the server first, enable Tun Mode, and then restart."));
                    neko_set_spmode_FAILED
                }
                if (!StartVPNProcess()) {
                    neko_set_spmode_FAILED
                }
            }
        } else {
            if (NekoGui::dataStore->vpn_internal_tun) {
                // current core is sing-box
            } else if (!sync) {
                // 内核已经自己退了（崩溃 / 被外部杀掉），没有要停的东西，直接落状态
                if (vpn_pid == 0) {
                    apply_state(false);
                    return;
                }

                // 用户主动取消勾选：走异步，UI 线程不能等（原来无参 waitForFinished =
                // 30 秒，密码框弹着的时候整窗口假死；用户点"取消"后 utun 与 72 条路由
                // 仍留着，界面却显示已关 → 整机断网）。
                if (vpn_stopping) {
                    MessageBoxWarning(software_name, tr("Tun is still shutting down, please wait."));
                    neko_set_spmode_FAILED
                }

                vpn_stopping = true;
                MW_show_log(">>>>>>>> " + tr("Stopping Tun process…"));

                // 这里 keep spmode_vpn = true，直到真正停成功。中途失败也必须保持 true，
                // 否则就是"谎报已关闭"。refresh_status() 会把勾选框重新勾上。
                refresh_status();
                StopVPNProcessAsync(false, [=](bool ok) {
                    vpn_stopping = false;
                    if (!ok) {
                        MW_show_log("<<<<<<<< " + tr("Failed to stop Tun process"));
                        MessageBoxWarning(
                            tr("Error"),
                            tr("Failed to stop the Tun process — Tun is still running.\n\n"
                               "Common causes:\n"
                               "• The administrator password prompt was cancelled.\n"
                               "• The kernel did not exit in time.\n\n"
                               "Tun stays enabled so you are not left with a half-broken network.\n"
                               "Try again, or use \"VPN Settings → Troubleshooting → Reset\"."));
                        refresh_status(); // 勾选框回到"已开启"，与实际状态一致
                        return;
                    }
                    MW_show_log("<<<<<<<< " + tr("Tun stopped"));
                    apply_state(false);
                });
                return;
            } else {
                // 退出链路：必须同步拿到结果，紧接着要读 spmode_vpn 决定能否继续退出。
                // 与用户主动取消不同——这里**停不掉也要把状态落成 false 并继续退出**：
                // 原来失败会走 neko_set_spmode_FAILED → spmode_vpn 仍为 true → 上层
                // "retry" 解锁后再无第二次触发，程序永远关不掉，而 utun 与 72 条路由
                // 还留在系统里（用户看到"点退出没反应 + 整机断网"）。
                // 停不掉是"已知且可恢复"的（下次启动 RecoverTunDNSIfNeeded 会救回来），
                // 关不掉才是死局。所以这里只告警，不阻塞。
                if (vpn_pid != 0 && !StopVPNProcess()) {
                    MW_show_log("[Error] " + tr("Failed to stop Tun process, continuing shutdown anyway. "
                                                "If the network stays broken after quitting, run "
                                                "\"VPN Settings → Troubleshooting → Reset\" next time."));
                }
                vpn_pid = 0;
            }
        }
    }

    apply_state(enable);
}

void MainWindow::refresh_status(const QString &traffic_update) {
    auto refresh_speed_label = [=] {
        if (traffic_update_cache == "") {
            ui->label_speed->setText(QObject::tr("Proxy: %1\nDirect: %2").arg("", ""));
        } else {
            ui->label_speed->setText(traffic_update_cache);
        }
    };

    // From TrafficLooper
    if (!traffic_update.isEmpty()) {
        traffic_update_cache = traffic_update;
        if (traffic_update == "STOP") {
            traffic_update_cache = "";
        } else {
            refresh_speed_label();
            return;
        }
    }

    refresh_speed_label();

    // From UI
    QString group_name;
    if (running != nullptr) {
        auto group = NekoGui::profileManager->GetGroup(running->gid);
        if (group != nullptr) group_name = group->name;
    }

    if (last_test_time.addSecs(2) < QTime::currentTime()) {
        auto txt = running == nullptr ? tr("Not Running")
                                      : QStringLiteral("[%1] %2").arg(group_name, running->bean->DisplayName()).left(30);
        ui->label_running->setText(txt);
    }
    //
    auto display_socks = DisplayAddress(NekoGui::dataStore->inbound_address, NekoGui::dataStore->inbound_socks_port);
    auto inbound_txt = QStringLiteral("Mixed: %1").arg(display_socks);
    ui->label_inbound->setText(inbound_txt);
    //
    ui->checkBox_VPN->setChecked(NekoGui::dataStore->spmode_vpn);
    ui->checkBox_SystemProxy->setChecked(NekoGui::dataStore->spmode_system_proxy);
    if (select_mode) {
        ui->label_running->setText(tr("Select") + " *");
        ui->label_running->setToolTip(tr("Select mode, double-click or press Enter to select a profile, press ESC to exit."));
    } else {
        ui->label_running->setToolTip({});
    }

    auto make_title = [=](bool isTray) {
        QStringList tt;
        if (!isTray && NekoGui::IsAdmin()) tt << "[Admin]";
        if (select_mode) tt << "[" + tr("Select") + "]";
        if (!title_error.isEmpty()) tt << "[" + title_error + "]";
        if (NekoGui::dataStore->spmode_vpn && !NekoGui::dataStore->spmode_system_proxy) tt << "[Tun]";
        if (!NekoGui::dataStore->spmode_vpn && NekoGui::dataStore->spmode_system_proxy) tt << "[" + tr("System Proxy") + "]";
        if (NekoGui::dataStore->spmode_vpn && NekoGui::dataStore->spmode_system_proxy) tt << "[Tun+" + tr("System Proxy") + "]";
        tt << software_name;
        if (!isTray) tt << "(" + QString(NKR_VERSION) + ")";
        if (!NekoGui::dataStore->active_routing.isEmpty() && NekoGui::dataStore->active_routing != "Default") {
            tt << "[" + NekoGui::dataStore->active_routing + "]";
        }
        if (running != nullptr) tt << running->bean->DisplayTypeAndName() + "@" + group_name;
        return tt.join(isTray ? "\n" : " ");
    };

    auto icon_status_new = Icon::NONE;

    if (running != nullptr) {
        if (NekoGui::dataStore->spmode_vpn) {
            icon_status_new = Icon::VPN;
        } else if (NekoGui::dataStore->spmode_system_proxy) {
            icon_status_new = Icon::SYSTEM_PROXY;
        } else {
            icon_status_new = Icon::RUNNING;
        }
    }

    // refresh title & window icon
    setWindowTitle(make_title(false));
    if (icon_status_new != icon_status) QApplication::setWindowIcon(Icon::GetTrayIcon(Icon::NONE));

    // refresh tray
    if (tray != nullptr) {
        tray->setToolTip(make_title(true));
        if (icon_status_new != icon_status) tray->setIcon(Icon::GetTrayQIcon(icon_status_new));
    }

    icon_status = icon_status_new;
}

// table显示

// refresh_groups -> show_group -> refresh_proxy_list
void MainWindow::refresh_groups() {
    NekoGui::dataStore->refreshing_group_list = true;

    // refresh group?
    for (int i = ui->tabWidget->count() - 1; i > 0; i--) {
        ui->tabWidget->removeTab(i);
    }

    int index = 0;
    for (const auto &gid: NekoGui::profileManager->groupsTabOrder) {
        auto group = NekoGui::profileManager->GetGroup(gid);
        if (index == 0) {
            ui->tabWidget->setTabText(0, group->name);
        } else {
            auto widget2 = new QWidget();
            auto layout2 = new QVBoxLayout();
            layout2->setContentsMargins(QMargins());
            layout2->setSpacing(0);
            widget2->setLayout(layout2);
            ui->tabWidget->addTab(widget2, group->name);
        }
        ui->tabWidget->tabBar()->setTabData(index, gid);
        index++;
    }

    // show after group changed
    if (NekoGui::profileManager->CurrentGroup() == nullptr) {
        NekoGui::dataStore->current_group = -1;
        ui->tabWidget->setCurrentIndex(groupId2TabIndex(0));
        show_group(NekoGui::profileManager->groupsTabOrder.count() > 0 ? NekoGui::profileManager->groupsTabOrder.first() : 0);
    } else {
        ui->tabWidget->setCurrentIndex(groupId2TabIndex(NekoGui::dataStore->current_group));
        show_group(NekoGui::dataStore->current_group);
    }

    NekoGui::dataStore->refreshing_group_list = false;
}

void MainWindow::refresh_proxy_list(const int &id) {
    refresh_proxy_list_impl(id, {});
}

void MainWindow::refresh_proxy_list_impl(const int &id, GroupSortAction groupSortAction) {
    // id < 0 重绘
    if (id < 0) {
        // 清空数据
        ui->proxyListTable->row2Id.clear();
        ui->proxyListTable->setRowCount(0);
        // 添加行
        int row = -1;
        for (const auto &[id, profile]: NekoGui::profileManager->profiles) {
            if (NekoGui::dataStore->current_group != profile->gid) continue;
            row++;
            ui->proxyListTable->insertRow(row);
            ui->proxyListTable->row2Id += id;
        }
    }

    // 显示排序
    if (id < 0) {
        switch (groupSortAction.method) {
            case GroupSortMethod::Raw: {
                auto group = NekoGui::profileManager->CurrentGroup();
                if (group == nullptr) return;
                ui->proxyListTable->order = group->order;
                break;
            }
            case GroupSortMethod::ById: {
                // Clear Order
                ui->proxyListTable->order.clear();
                ui->proxyListTable->callback_save_order();
                break;
            }
            case GroupSortMethod::ByAddress:
            case GroupSortMethod::ByName:
            case GroupSortMethod::ByLatency:
            case GroupSortMethod::ByType: {
                std::sort(ui->proxyListTable->order.begin(), ui->proxyListTable->order.end(),
                          [=](int a, int b) {
                              QString ms_a;
                              QString ms_b;
                              if (groupSortAction.method == GroupSortMethod::ByType) {
                                  ms_a = NekoGui::profileManager->GetProfile(a)->bean->DisplayType();
                                  ms_b = NekoGui::profileManager->GetProfile(b)->bean->DisplayType();
                              } else if (groupSortAction.method == GroupSortMethod::ByName) {
                                  ms_a = NekoGui::profileManager->GetProfile(a)->bean->name;
                                  ms_b = NekoGui::profileManager->GetProfile(b)->bean->name;
                              } else if (groupSortAction.method == GroupSortMethod::ByAddress) {
                                  ms_a = NekoGui::profileManager->GetProfile(a)->bean->DisplayAddress();
                                  ms_b = NekoGui::profileManager->GetProfile(b)->bean->DisplayAddress();
                              } else if (groupSortAction.method == GroupSortMethod::ByLatency) {
                                  ms_a = NekoGui::profileManager->GetProfile(a)->full_test_report;
                                  ms_b = NekoGui::profileManager->GetProfile(b)->full_test_report;
                              }
                              auto get_latency_for_sort = [](int id) {
                                  auto i = NekoGui::profileManager->GetProfile(id)->latency;
                                  if (i == 0) i = 100000;
                                  if (i < 0) i = 99999;
                                  return i;
                              };
                              if (groupSortAction.descending) {
                                  if (groupSortAction.method == GroupSortMethod::ByLatency) {
                                      if (ms_a.isEmpty() && ms_b.isEmpty()) {
                                          // compare latency if full_test_report is empty
                                          return get_latency_for_sort(a) > get_latency_for_sort(b);
                                      }
                                  }
                                  return ms_a > ms_b;
                              } else {
                                  if (groupSortAction.method == GroupSortMethod::ByLatency) {
                                      auto int_a = NekoGui::profileManager->GetProfile(a)->latency;
                                      auto int_b = NekoGui::profileManager->GetProfile(b)->latency;
                                      if (ms_a.isEmpty() && ms_b.isEmpty()) {
                                          // compare latency if full_test_report is empty
                                          return get_latency_for_sort(a) < get_latency_for_sort(b);
                                      }
                                  }
                                  return ms_a < ms_b;
                              }
                          });
                break;
            }
        }
        ui->proxyListTable->update_order(groupSortAction.save_sort);
    }

    // refresh data
    refresh_proxy_list_impl_refresh_data(id);
}

void MainWindow::refresh_proxy_list_impl_refresh_data(const int &id) {
    // 绘制或更新item(s)
    for (int row = 0; row < ui->proxyListTable->rowCount(); row++) {
        auto profileId = ui->proxyListTable->row2Id[row];
        if (id >= 0 && profileId != id) continue; // refresh ONE item
        auto profile = NekoGui::profileManager->GetProfile(profileId);
        if (profile == nullptr) continue;

        auto isRunning = profileId == NekoGui::dataStore->started_id;
        auto f0 = std::make_unique<QTableWidgetItem>();
        f0->setData(114514, profileId);

        // Check state
        auto check = f0->clone();
        check->setText(isRunning ? "✓" : Int2String(row + 1));
        ui->proxyListTable->setVerticalHeaderItem(row, check);

        // C0: Type
        auto f = f0->clone();
        f->setText(profile->bean->DisplayType());
        if (isRunning) f->setForeground(palette().link());
        ui->proxyListTable->setItem(row, 0, f);

        // C1: Address+Port
        f = f0->clone();
        f->setText(profile->bean->DisplayAddress());
        if (isRunning) f->setForeground(palette().link());
        ui->proxyListTable->setItem(row, 1, f);

        // C2: Name
        f = f0->clone();
        f->setText(profile->bean->name);
        if (isRunning) f->setForeground(palette().link());
        ui->proxyListTable->setItem(row, 2, f);

        // C3: Test Result
        f = f0->clone();
        if (profile->full_test_report.isEmpty()) {
            auto color = profile->DisplayLatencyColor();
            if (color.isValid()) f->setForeground(color);
            f->setText(profile->DisplayLatency());
        } else {
            f->setText(profile->full_test_report);
        }
        ui->proxyListTable->setItem(row, 3, f);

        // C4: Traffic
        f = f0->clone();
        f->setText(profile->traffic_data->DisplayTraffic());
        ui->proxyListTable->setItem(row, 4, f);
    }
}

// table菜单相关

void MainWindow::on_proxyListTable_itemDoubleClicked(QTableWidgetItem *item) {
    auto id = item->data(114514).toInt();
    if (select_mode) {
        emit profile_selected(id);
        select_mode = false;
        refresh_status();
        return;
    }
    auto dialog = new DialogEditProfile("", id, this);
    connect(dialog, &QDialog::finished, dialog, &QDialog::deleteLater);
}

void MainWindow::on_menu_add_from_input_triggered() {
    auto dialog = new DialogEditProfile("socks", NekoGui::dataStore->current_group, this);
    connect(dialog, &QDialog::finished, dialog, &QDialog::deleteLater);
}

void MainWindow::on_menu_add_from_clipboard_triggered() {
    auto clipboard = QApplication::clipboard()->text();
    NekoGui_sub::groupUpdater->AsyncUpdate(clipboard);
}

void MainWindow::on_menu_clone_triggered() {
    auto ents = get_now_selected_list();
    if (ents.isEmpty()) return;

    auto btn = QMessageBox::question(this, tr("Clone"), tr("Clone %1 item(s)").arg(ents.count()));
    if (btn != QMessageBox::Yes) return;

    QStringList sls;
    for (const auto &ent: ents) {
        sls << ent->bean->ToNekorayShareLink(ent->type);
    }

    NekoGui_sub::groupUpdater->AsyncUpdate(sls.join("\n"));
}

void MainWindow::on_menu_move_triggered() {
    auto ents = get_now_selected_list();
    if (ents.isEmpty()) return;

    auto items = QStringList{};
    for (auto gid: NekoGui::profileManager->groupsTabOrder) {
        auto group = NekoGui::profileManager->GetGroup(gid);
        if (group == nullptr) continue;
        items += Int2String(gid) + " " + group->name;
    }

    bool ok;
    auto a = QInputDialog::getItem(nullptr,
                                   tr("Move"),
                                   tr("Move %1 item(s)").arg(ents.count()),
                                   items, 0, false, &ok);
    if (!ok) return;
    auto gid = SubStrBefore(a, " ").toInt();
    for (const auto &ent: ents) {
        NekoGui::profileManager->MoveProfile(ent, gid);
    }
    refresh_proxy_list();
}

void MainWindow::on_menu_delete_triggered() {
    auto ents = get_now_selected_list();
    if (ents.count() == 0) return;
    if (QMessageBox::question(this, tr("Confirmation"), QString(tr("Remove %1 item(s) ?")).arg(ents.count())) ==
        QMessageBox::StandardButton::Yes) {
        for (const auto &ent: ents) {
            NekoGui::profileManager->DeleteProfile(ent->id);
        }
        refresh_proxy_list();
    }
}

void MainWindow::on_menu_reset_traffic_triggered() {
    auto ents = get_now_selected_list();
    if (ents.count() == 0) return;
    for (const auto &ent: ents) {
        ent->traffic_data->Reset();
        ent->Save();
        refresh_proxy_list(ent->id);
    }
}

void MainWindow::on_menu_profile_debug_info_triggered() {
    auto ents = get_now_selected_list();
    if (ents.count() != 1) return;
    auto btn = QMessageBox::information(this, software_name, ents.first()->ToJsonBytes(), "OK", "Edit", "Reload", 0, 0);
    if (btn == 1) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QStringLiteral("profiles/%1.json").arg(ents.first()->id)).absoluteFilePath()));
    } else if (btn == 2) {
        NekoGui::dataStore->Load();
        NekoGui::profileManager->LoadManager();
        refresh_proxy_list();
    }
}

void MainWindow::on_menu_copy_links_triggered() {
    if (ui->masterLogBrowser->hasFocus()) {
        ui->masterLogBrowser->copy();
        return;
    }
    auto ents = get_now_selected_list();
    QStringList links;
    for (const auto &ent: ents) {
        links += ent->bean->ToShareLink();
    }
    if (links.length() == 0) return;
    QApplication::clipboard()->setText(links.join("\n"));
    show_log_impl(tr("Copied %1 item(s)").arg(links.length()));
}

void MainWindow::on_menu_copy_links_nkr_triggered() {
    auto ents = get_now_selected_list();
    QStringList links;
    for (const auto &ent: ents) {
        links += ent->bean->ToNekorayShareLink(ent->type);
    }
    if (links.length() == 0) return;
    QApplication::clipboard()->setText(links.join("\n"));
    show_log_impl(tr("Copied %1 item(s)").arg(links.length()));
}

void MainWindow::on_menu_export_config_triggered() {
    auto ents = get_now_selected_list();
    if (ents.count() != 1) return;
    auto ent = ents.first();
    if (ent->bean->DisplayCoreType() != software_core_name) return;

    auto result = BuildConfig(ent, false, true);
    QString config_core = QJsonObject2QString(result->coreConfig, false);
    QApplication::clipboard()->setText(config_core);

    QMessageBox msg(QMessageBox::Information, tr("Config copied"), tr("Config copied"));
    msg.addButton("Copy core config", QMessageBox::YesRole);
    msg.addButton("Copy test config", QMessageBox::NoRole);
    msg.addButton(QMessageBox::Ok);
    msg.setEscapeButton(QMessageBox::Ok);
    msg.setDefaultButton(QMessageBox::Ok);
    auto ret = msg.exec();
    if (ret == 2) {
        result = BuildConfig(ent, false, false);
        config_core = QJsonObject2QString(result->coreConfig, false);
        QApplication::clipboard()->setText(config_core);
    } else if (ret == 3) {
        result = BuildConfig(ent, true, false);
        config_core = QJsonObject2QString(result->coreConfig, false);
        QApplication::clipboard()->setText(config_core);
    }
}

void MainWindow::display_qr_link(bool nkrFormat) {
    auto ents = get_now_selected_list();
    if (ents.count() != 1) return;

    class W : public QDialog {
    public:
        QLabel *l = nullptr;
        QCheckBox *cb = nullptr;
        //
        QPlainTextEdit *l2 = nullptr;
        QImage im;
        //
        QString link;
        QString link_nk;

        void show_qr(const QSize &size) const {
            auto side = size.height() - 20 - l2->size().height() - cb->size().height();
            l->setPixmap(QPixmap::fromImage(im.scaled(side, side, Qt::KeepAspectRatio, Qt::FastTransformation),
                                            Qt::MonoOnly));
            l->resize(side, side);
        }

        void refresh(bool is_nk) {
            auto link_display = is_nk ? link_nk : link;
            l2->setPlainText(link_display);
            constexpr qint32 qr_padding = 2;
            //
            try {
                qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(link_display.toUtf8().data(), qrcodegen::QrCode::Ecc::MEDIUM);
                qint32 sz = qr.getSize();
                im = QImage(sz + qr_padding * 2, sz + qr_padding * 2, QImage::Format_RGB32);
                QRgb black = qRgb(0, 0, 0);
                QRgb white = qRgb(255, 255, 255);
                im.fill(white);
                for (int y = 0; y < sz; y++)
                    for (int x = 0; x < sz; x++)
                        if (qr.getModule(x, y))
                            im.setPixel(x + qr_padding, y + qr_padding, black);
                show_qr(size());
            } catch (const std::exception &ex) {
                QMessageBox::warning(nullptr, "error", ex.what());
            }
        }

        W(const QString &link_, const QString &link_nk_) {
            link = link_;
            link_nk = link_nk_;
            //
            setLayout(new QVBoxLayout);
            setMinimumSize(256, 256);
            QSizePolicy sizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            sizePolicy.setHeightForWidth(true);
            setSizePolicy(sizePolicy);
            //
            l = new QLabel();
            l->setMinimumSize(256, 256);
            l->setMargin(6);
            l->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            l->setScaledContents(true);
            layout()->addWidget(l);
            cb = new QCheckBox;
            cb->setText("Neko Links");
            layout()->addWidget(cb);
            l2 = new QPlainTextEdit();
            l2->setReadOnly(true);
            layout()->addWidget(l2);
            //
            connect(cb, &QCheckBox::toggled, this, &W::refresh);
            refresh(false);
        }

        void resizeEvent(QResizeEvent *resizeEvent) override {
            show_qr(resizeEvent->size());
        }
    };

    auto link = ents.first()->bean->ToShareLink();
    auto link_nk = ents.first()->bean->ToNekorayShareLink(ents.first()->type);
    auto w = new W(link, link_nk);
    w->setWindowTitle(ents.first()->bean->DisplayTypeAndName());
    w->exec();
    w->deleteLater();
}

void MainWindow::on_menu_scan_qr_triggered() {
#ifndef NKR_NO_ZXING
    using namespace ZXingQt;

    // macOS 10.15+ 没有"屏幕录制"权限时，grabWindow 不报错，只返回一张**把窗口内容
    // 整块剔除**的图（只剩壁纸）→ ReadBarcode 恒为空 → 每次点都弹 "QR Code not found"。
    // 所以先查权限，别让用户以为二维码坏了。
#ifdef Q_OS_MACOS
    if (!NekoGui_mac::HasScreenCaptureAccess()) {
        QMessageBox box(QMessageBox::Question, software_name,
                        tr("Scanning a QR code from the screen needs the \"Screen Recording\" permission.\n\n"
                           "Grant it in System Settings → Privacy & Security → Screen Recording, "
                           "then restart RekoBox and try again.\n\n"
                           "Tip: \"Add profile from clipboard\" works without any permission."),
                        QMessageBox::NoButton, GetMessageBoxParent());
        auto openBtn = box.addButton(tr("Open System Settings"), QMessageBox::AcceptRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == openBtn) {
            // 弹系统授权框（把本 App 加进列表）；真正的权限要重启后才生效
            NekoGui_mac::RequestScreenCaptureAccess();
            QDesktopServices::openUrl(
                QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"));
        }
        return;
    }
#endif

    hide();
    QThread::sleep(1);

    auto screen = QGuiApplication::primaryScreen();
    auto geom = screen->geometry();
    auto qpx = screen->grabWindow(0, geom.x(), geom.y(), geom.width(), geom.height());

    show();

    auto hints = DecodeHints()
                     .setFormats(BarcodeFormat::QRCode)
                     .setTryRotate(false)
                     .setBinarizer(Binarizer::FixedThreshold);

    auto result = ReadBarcode(qpx.toImage(), hints);
    const auto &text = result.text();
    if (text.isEmpty()) {
#ifdef Q_OS_MACOS
        MessageBoxInfo(software_name,
                       tr("QR Code not found") + "\n\n" +
                           tr("If the QR code was on another app's window, check that "
                              "RekoBox still has the \"Screen Recording\" permission — "
                              "after re-granting it the app must be restarted."));
#else
        MessageBoxInfo(software_name, tr("QR Code not found"));
#endif
    } else {
        show_log_impl("QR Code Result:\n" + text);
        NekoGui_sub::groupUpdater->AsyncUpdate(text);
    }
#endif
}

void MainWindow::on_menu_clear_test_result_triggered() {
    for (const auto &profile: get_selected_or_group()) {
        profile->latency = 0;
        profile->full_test_report = "";
        profile->Save();
    }
    refresh_proxy_list();
}

void MainWindow::on_menu_select_all_triggered() {
    if (ui->masterLogBrowser->hasFocus()) {
        ui->masterLogBrowser->selectAll();
        return;
    }
    ui->proxyListTable->selectAll();
}

void MainWindow::on_menu_delete_repeat_triggered() {
    QList<std::shared_ptr<NekoGui::ProxyEntity>> out;
    QList<std::shared_ptr<NekoGui::ProxyEntity>> out_del;

    NekoGui::ProfileFilter::Uniq(NekoGui::profileManager->CurrentGroup()->Profiles(), out, true, false);
    NekoGui::ProfileFilter::OnlyInSrc_ByPointer(NekoGui::profileManager->CurrentGroup()->Profiles(), out, out_del);

    int remove_display_count = 0;
    QString remove_display;
    for (const auto &ent: out_del) {
        remove_display += ent->bean->DisplayTypeAndName() + "\n";
        if (++remove_display_count == 20) {
            remove_display += "...";
            break;
        }
    }

    if (out_del.length() > 0 &&
        QMessageBox::question(this, tr("Confirmation"), tr("Remove %1 item(s) ?").arg(out_del.length()) + "\n" + remove_display) == QMessageBox::StandardButton::Yes) {
        for (const auto &ent: out_del) {
            NekoGui::profileManager->DeleteProfile(ent->id);
        }
        refresh_proxy_list();
    }
}

bool mw_sub_updating = false;

void MainWindow::on_menu_update_subscription_triggered() {
    auto group = NekoGui::profileManager->CurrentGroup();
    if (group->url.isEmpty()) return;
    if (mw_sub_updating) return;
    mw_sub_updating = true;
    NekoGui_sub::groupUpdater->AsyncUpdate(group->url, group->id, [&] { mw_sub_updating = false; });
}

void MainWindow::on_menu_remove_unavailable_triggered() {
    QList<std::shared_ptr<NekoGui::ProxyEntity>> out_del;

    for (const auto &[_, profile]: NekoGui::profileManager->profiles) {
        if (NekoGui::dataStore->current_group != profile->gid) continue;
        if (profile->latency < 0) out_del += profile;
    }

    int remove_display_count = 0;
    QString remove_display;
    for (const auto &ent: out_del) {
        remove_display += ent->bean->DisplayTypeAndName() + "\n";
        if (++remove_display_count == 20) {
            remove_display += "...";
            break;
        }
    }

    if (out_del.length() > 0 &&
        QMessageBox::question(this, tr("Confirmation"), tr("Remove %1 item(s) ?").arg(out_del.length()) + "\n" + remove_display) == QMessageBox::StandardButton::Yes) {
        for (const auto &ent: out_del) {
            NekoGui::profileManager->DeleteProfile(ent->id);
        }
        refresh_proxy_list();
    }
}

void MainWindow::on_menu_resolve_domain_triggered() {
    auto profiles = get_selected_or_group();
    if (profiles.isEmpty()) return;

    if (QMessageBox::question(this,
                              tr("Confirmation"),
                              tr("Resolving domain to IP, if support.")) != QMessageBox::StandardButton::Yes) {
        return;
    }
    if (mw_sub_updating) return;
    mw_sub_updating = true;
    NekoGui::dataStore->resolve_count = profiles.count();

    for (const auto &profile: profiles) {
        profile->bean->ResolveDomainToIP([=] {
            profile->Save();
            if (--NekoGui::dataStore->resolve_count != 0) return;
            refresh_proxy_list();
            mw_sub_updating = false;
        });
    }
}

void MainWindow::on_proxyListTable_customContextMenuRequested(const QPoint &pos) {
    ui->menu_server->popup(ui->proxyListTable->viewport()->mapToGlobal(pos)); // 弹出菜单
}

QList<std::shared_ptr<NekoGui::ProxyEntity>> MainWindow::get_now_selected_list() {
    auto items = ui->proxyListTable->selectedItems();
    QList<std::shared_ptr<NekoGui::ProxyEntity>> list;
    for (auto item: items) {
        auto id = item->data(114514).toInt();
        auto ent = NekoGui::profileManager->GetProfile(id);
        if (ent != nullptr && !list.contains(ent)) list += ent;
    }
    return list;
}

QList<std::shared_ptr<NekoGui::ProxyEntity>> MainWindow::get_selected_or_group() {
    auto selected_or_group = ui->menu_server->property("selected_or_group").toInt();
    QList<std::shared_ptr<NekoGui::ProxyEntity>> profiles;
    if (selected_or_group > 0) {
        profiles = get_now_selected_list();
        if (profiles.isEmpty() && selected_or_group == 2) profiles = NekoGui::profileManager->CurrentGroup()->ProfilesWithOrder();
    } else {
        profiles = NekoGui::profileManager->CurrentGroup()->ProfilesWithOrder();
    }
    return profiles;
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    switch (event->key()) {
        case Qt::Key_Escape:
            // take over by shortcut_esc
            break;
        case Qt::Key_Enter:
            neko_start();
            break;
        default:
            QMainWindow::keyPressEvent(event);
    }
}

// Log

inline void FastAppendTextDocument(const QString &message, QTextDocument *doc) {
    QTextCursor cursor(doc);
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    cursor.insertBlock();
    cursor.insertText(message);
    cursor.endEditBlock();
}

void MainWindow::show_log_impl(const QString &log) {
    auto lines = SplitLines(log.trimmed());
    if (lines.isEmpty()) return;

    QStringList newLines;
    auto log_ignore = NekoGui::dataStore->log_ignore;
    for (const auto &line: lines) {
        bool showThisLine = true;
        for (const auto &str: log_ignore) {
            if (line.contains(str)) {
                showThisLine = false;
                break;
            }
        }
#ifdef Q_OS_MACOS
        // TUN 开着时整机流量都进内核，info 级别下面板会被"正常连接"淹没
        // （实测 240 行/秒，5125 行里零错误，其中 1563 行是同一个目标在重试 QUIC）。
        //
        // P1-11：原来是**硬编码**丢弃，与 Windows 不一致，用户看不到"正在连接哪个节点"，
        // 排障时容易误判成"没反应"。现在改成显式级别选择器（三平台共用），
        // TUN 模式下默认收紧到"警告及以上"，需要看细节时用户自己调回"全部"。
        if (showThisLine && NekoGui::dataStore->spmode_vpn && NekoGui::dataStore->log_panel_level == 0) {
            const auto lower = line.toLower();
            const bool keep = lower.contains("error") || lower.contains("warn") ||
                              lower.contains("panic") || lower.contains("fatal") ||
                              line.startsWith("[mac]") || line.startsWith("[tun]") ||
                              line.startsWith(">>>>>>>>") || line.startsWith("<<<<<<<<");
            if (!keep) showThisLine = false;
        }
#endif
        // 级别过滤（显式选择的，优先于上面的默认策略）
        if (showThisLine && NekoGui::dataStore->log_panel_level == 1) {
            const auto lower = line.toLower();
            if (!(lower.contains("error") || lower.contains("warn") || lower.contains("panic") ||
                  lower.contains("fatal") || line.startsWith(">>>>>>>>") || line.startsWith("<<<<<<<<"))) {
                showThisLine = false;
            }
        } else if (showThisLine && NekoGui::dataStore->log_panel_level == 2) {
            const auto lower = line.toLower();
            if (!(lower.contains("error") || lower.contains("panic") || lower.contains("fatal"))) {
                showThisLine = false;
            }
        }
        if (showThisLine) newLines << line;
    }
    if (newLines.isEmpty()) return;

    FastAppendTextDocument(newLines.join("\n"), qvLogDocument);
    // qvLogDocument->setPlainText(qvLogDocument->toPlainText() + log);
    // From https://gist.github.com/jemyzhang/7130092
    auto block = qvLogDocument->begin();

    while (block.isValid()) {
        if (qvLogDocument->blockCount() > NekoGui::dataStore->max_log_line) {
            QTextCursor cursor(block);
            block = block.next();
            cursor.select(QTextCursor::BlockUnderCursor);
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            continue;
        }
        break;
    }
}

#define ADD_TO_CURRENT_ROUTE(a, b)                                                                   \
    NekoGui::dataStore->routing->a = (SplitLines(NekoGui::dataStore->routing->a) << (b)).join("\n"); \
    NekoGui::dataStore->routing->Save();

void MainWindow::on_masterLogBrowser_customContextMenuRequested(const QPoint &pos) {
    QMenu *menu = ui->masterLogBrowser->createStandardContextMenu();

    auto sep = new QAction(this);
    sep->setSeparator(true);
    menu->addAction(sep);

    // 日志级别（P1-11）：MAC 原来在 TUN 下硬拒绝 info 级别的行，用户看不到
    // "正在连接哪个节点"，排障时以为没反应。现在给显式开关，三平台一致。
    auto level_menu = menu->addMenu(tr("Log level"));
    auto add_level = [=](const QString &text, int value) {
        auto a = level_menu->addAction(text);
        a->setCheckable(true);
        a->setChecked(NekoGui::dataStore->log_panel_level == value);
        connect(a, &QAction::triggered, this, [=] {
            NekoGui::dataStore->log_panel_level = value;
            NekoGui::dataStore->Save();
            MW_show_log(tr("[app] log level → %1").arg(text));
        });
    };
    add_level(tr("All (verbose)"), 0);
    add_level(tr("Warnings and errors"), 1);
    add_level(tr("Errors only"), 2);

    sep = new QAction(this);
    sep->setSeparator(true);
    menu->addAction(sep);

    auto action_add_ignore = new QAction(this);
    action_add_ignore->setText(tr("Set ignore keyword"));
    connect(action_add_ignore, &QAction::triggered, this, [=] {
        auto list = NekoGui::dataStore->log_ignore;
        auto newStr = ui->masterLogBrowser->textCursor().selectedText().trimmed();
        if (!newStr.isEmpty()) list << newStr;
        bool ok;
        newStr = QInputDialog::getMultiLineText(GetMessageBoxParent(), tr("Set ignore keyword"), tr("Set the following keywords to ignore?\nSplit by line."), list.join("\n"), &ok);
        if (ok) {
            NekoGui::dataStore->log_ignore = SplitLines(newStr);
            NekoGui::dataStore->Save();
        }
    });
    menu->addAction(action_add_ignore);

    auto action_add_route = new QAction(this);
    action_add_route->setText(tr("Save as route"));
    connect(action_add_route, &QAction::triggered, this, [=] {
        auto newStr = ui->masterLogBrowser->textCursor().selectedText().trimmed();
        if (newStr.isEmpty()) return;
        //
        bool ok;
        newStr = QInputDialog::getText(GetMessageBoxParent(), tr("Save as route"), tr("Edit"), {}, newStr, &ok).trimmed();
        if (!ok) return;
        if (newStr.isEmpty()) return;
        //
        auto select = IsIpAddress(newStr) ? 0 : 3;
        QStringList items = {"proxyIP", "bypassIP", "blockIP", "proxyDomain", "bypassDomain", "blockDomain"};
        auto item = QInputDialog::getItem(GetMessageBoxParent(), tr("Save as route"),
                                          tr("Save \"%1\" as a routing rule?").arg(newStr),
                                          items, select, false, &ok);
        if (ok) {
            auto index = items.indexOf(item);
            switch (index) {
                case 0:
                    ADD_TO_CURRENT_ROUTE(proxy_ip, newStr);
                    break;
                case 1:
                    ADD_TO_CURRENT_ROUTE(direct_ip, newStr);
                    break;
                case 2:
                    ADD_TO_CURRENT_ROUTE(block_ip, newStr);
                    break;
                case 3:
                    ADD_TO_CURRENT_ROUTE(proxy_domain, newStr);
                    break;
                case 4:
                    ADD_TO_CURRENT_ROUTE(direct_domain, newStr);
                    break;
                case 5:
                    ADD_TO_CURRENT_ROUTE(block_domain, newStr);
                    break;
                default:
                    break;
            }
            MW_dialog_message("", "UpdateDataStore,RouteChanged");
        }
    });
    menu->addAction(action_add_route);

    auto action_clear = new QAction(this);
    action_clear->setText(tr("Clear"));
    connect(action_clear, &QAction::triggered, this, [=] {
        qvLogDocument->clear();
        ui->masterLogBrowser->clear();
    });
    menu->addAction(action_clear);

    menu->exec(ui->masterLogBrowser->viewport()->mapToGlobal(pos)); // 弹出菜单
}

// eventFilter

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress) {
        auto mouseEvent = dynamic_cast<QMouseEvent *>(event);
        if (obj == ui->label_running && mouseEvent->button() == Qt::LeftButton && running != nullptr) {
            speedtest_current();
            return true;
        } else if (obj == ui->label_inbound && mouseEvent->button() == Qt::LeftButton) {
            on_menu_basic_settings_triggered();
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        if (obj == ui->splitter) {
            auto size = ui->splitter->size();
            ui->splitter->setSizes({size.height() / 2, size.height() / 2});
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// profile selector

void MainWindow::start_select_mode(QObject *context, const std::function<void(int)> &callback) {
    select_mode = true;
    connectOnce(this, &MainWindow::profile_selected, context, callback);
    refresh_status();
}

// 连接列表

inline QJsonArray last_arr; // format is nekoray_connections_json

void MainWindow::refresh_connection_list(const QJsonArray &arr) {
    if (last_arr == arr) {
        return;
    }
    last_arr = arr;

    if (NekoGui::dataStore->flag_debug) qDebug() << arr;

    ui->tableWidget_conn->setRowCount(0);

    int row = -1;
    for (const auto &_item: arr) {
        auto item = _item.toObject();
        if (NekoGui::dataStore->ignoreConnTag.contains(item["Tag"].toString())) continue;

        row++;
        ui->tableWidget_conn->insertRow(row);

        auto f0 = std::make_unique<QTableWidgetItem>();
        f0->setData(114514, item["ID"].toInt());

        // C0: Status
        auto c0 = new QLabel;
        auto start_t = item["Start"].toInt();
        auto end_t = item["End"].toInt();
        // icon
        auto outboundTag = item["Tag"].toString();
        if (outboundTag == "block") {
            c0->setPixmap(Icon::GetMaterialIcon("cancel"));
        } else {
            if (end_t > 0) {
                c0->setPixmap(Icon::GetMaterialIcon("history"));
            } else {
                c0->setPixmap(Icon::GetMaterialIcon("swap-vertical"));
            }
        }
        c0->setAlignment(Qt::AlignCenter);
        c0->setToolTip(tr("Start: %1\nEnd: %2").arg(DisplayTime(start_t), end_t > 0 ? DisplayTime(end_t) : ""));
        ui->tableWidget_conn->setCellWidget(row, 0, c0);

        // C1: Outbound
        auto f = f0->clone();
        f->setToolTip("");
        f->setText(outboundTag);
        ui->tableWidget_conn->setItem(row, 1, f);

        // C2: Destination
        f = f0->clone();
        QString target1 = item["Dest"].toString();
        QString target2 = item["RDest"].toString();
        if (target2.isEmpty() || target1 == target2) {
            target2 = "";
        }
        f->setText("[" + target1 + "] " + target2);
        ui->tableWidget_conn->setItem(row, 2, f);
    }
}

// Hotkey

#ifndef NKR_NO_QHOTKEY

#include <QHotkey>

inline QList<std::shared_ptr<QHotkey>> RegisteredHotkey;

void MainWindow::RegisterHotkey(bool unregister) {
    while (!RegisteredHotkey.isEmpty()) {
        auto hk = RegisteredHotkey.takeFirst();
        hk->deleteLater();
    }
    hotkey_failures.clear();
    if (unregister) return;

    // 名字只用于提示文案，顺序与下面的 regstr 一一对应
    const QStringList names{
        tr("Show main window"),
        tr("Show groups"),
        tr("Show routes"),
        tr("System proxy menu"),
    };

    QStringList regstr{
        NekoGui::dataStore->hotkey_mainwindow,
        NekoGui::dataStore->hotkey_group,
        NekoGui::dataStore->hotkey_route,
        NekoGui::dataStore->hotkey_system_proxy_menu,
    };

    for (int i = 0; i < regstr.size(); i++) {
        const auto &key = regstr[i];
        if (key.isEmpty()) continue;
        if (regstr.count(key) > 1) {
            hotkey_failures << tr("%1: %2 (duplicated)").arg(names[i], key);
            continue; // 原来这里是 return，会让后面所有热键一起不注册
        }
        QKeySequence k(key);
        if (k.isEmpty()) {
            hotkey_failures << tr("%1: %2 (cannot be parsed)").arg(names[i], key);
            continue;
        }
        auto hk = std::make_shared<QHotkey>(k, true);
        if (hk->isRegistered()) {
            RegisteredHotkey += hk;
            connect(hk.get(), &QHotkey::activated, this, [=] { HotkeyEvent(key); });
        } else {
            // 注册不上通常是"已被别的 App 或系统菜单占用"。
            // 原来这里静默 deleteLater，用户按了没反应却没有任何线索。
            // 用 NativeText 显示，mac 上才会写成 ⌘S 而不是 Ctrl+S，否则用户按提示
            // 去试的键位是错的（Qt 在 mac 把物理 Command 映射成 Qt::CTRL，
            // 所以存储的 "Ctrl+S" 在 mac 上就是 ⌘S）。
            hotkey_failures << tr("%1: %2 (already used by another app)").arg(names[i], k.toString(QKeySequence::NativeText));
            if (hk) hk->deleteLater();
        }
    }

    for (const auto &f: hotkey_failures) {
        qWarning() << "[hotkey] register failed:" << f;
    }
}

QString MainWindow::HotkeyFailureSummary() const {
    return hotkey_failures.join("\n");
}

void MainWindow::HotkeyEvent(const QString &key) {
    if (key.isEmpty()) return;
    runOnUiThread([=] {
        if (key == NekoGui::dataStore->hotkey_mainwindow) {
            tray->activated(QSystemTrayIcon::ActivationReason::Trigger);
        } else if (key == NekoGui::dataStore->hotkey_group) {
            on_menu_manage_groups_triggered();
        } else if (key == NekoGui::dataStore->hotkey_route) {
            on_menu_routing_settings_triggered();
        } else if (key == NekoGui::dataStore->hotkey_system_proxy_menu) {
            ui->menu_spmode->popup(QCursor::pos());
        }
    });
}

#else

void MainWindow::RegisterHotkey(bool unregister) {}

void MainWindow::HotkeyEvent(const QString &key) {}

#endif

// VPN Launcher

bool MainWindow::StartVPNProcess() {
    //
    if (vpn_pid != 0) {
        return true;
    }
#ifdef Q_OS_MACOS
    // 启动前清掉上一次的"请停"标记：正常退出会删掉，但崩溃/强杀会留下。
    // 留着的后果是——新 tun 脚本刚起来就被自己那个哨兵看到标记而立刻退掉。
    QFile::remove(QDir::current().absoluteFilePath("tun-stop-request"));
#endif
    //
    // 节点地址要传给 tun 配置生成：它得被排除在 tun 路由外，否则主内核连节点会回环
    auto configPath = NekoGui::WriteVPNSingBoxConfig(running != nullptr ? running->bean->serverAddress : QString());
    auto scriptPath = NekoGui::WriteVPNLinuxScript(configPath);
    //
#ifdef Q_OS_WIN
    runOnNewThread([=] {
        vpn_pid = 1; // TODO get pid?
        WinCommander::runProcessElevated(QApplication::applicationDirPath() + "/rekobox_core.exe",
                                         {"--disable-color", "run", "-c", configPath}, "",
                                         NekoGui::dataStore->vpn_hide_console ? WinCommander::SW_HIDE : WinCommander::SW_SHOWMINIMIZED); // blocking
        vpn_pid = 0;
        runOnUiThread([=] { neko_set_spmode_vpn(false); });
    });
#else
    //
    auto vpn_process = new QProcess;
    QProcess::connect(vpn_process, &QProcess::stateChanged, this, [=](QProcess::ProcessState state) {
        if (state == QProcess::NotRunning) {
            vpn_pid = 0;
            vpn_process->deleteLater();
            GetMainWindow()->neko_set_spmode_vpn(false);
        }
    });
    //
    // 之前用 ForwardedChannels，TUN 内核的输出直接进了 App 的 stderr——从 Finder 启动时
    // 等于扔进黑洞，出问题只能靠猜（已经吃过两次亏）。这里改成自己收：
    // 全量写 config/neko-tun.log 便于事后查，面板只回显错误/告警，避免刷屏。
    vpn_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(vpn_process, &QProcess::readyRead, this, [vpn_process] {
        const auto output = QString::fromUtf8(vpn_process->readAll());
        if (output.isEmpty()) return;

        QFile log(QDir::current().absoluteFilePath("neko-tun.log"));
        if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            log.write(output.toUtf8());
        }

        for (const auto &line: output.split('\n', Qt::SkipEmptyParts)) {
            const auto lower = line.toLower();
            if (lower.contains("error") || lower.contains("panic") || lower.contains("fatal") ||
                lower.contains("warn")) {
                // 内核带 --disable-color 时一般没有转义序列，但老配置/手工启动的
                // 内核会带 ANSI 颜色码，面板上会显示成 "[0m" 之类的乱码。
                MW_show_log("[tun] " + cleanVT100String(line).left(300));
            }
        }
    });
#ifdef Q_OS_MACOS
    // 两层转义：shell 单引号（路径可能有空格）→ AppleScript 双引号字符串
    const auto shellCmd = "bash " + NekoGui_mac::ShellQuote(scriptPath);
    vpn_process->start("osascript",
                       {"-e", QStringLiteral("do shell script \"%1\" with administrator privileges")
                                      .arg(NekoGui_mac::AppleScriptString(shellCmd))});
#else
    vpn_process->start("pkexec", {"bash", scriptPath});
#endif
    vpn_process->waitForStarted();
    vpn_pid = vpn_process->processId(); // actually it's pkexec or bash PID

#ifdef Q_OS_MACOS
    // 接管系统 DNS（P1-8）。
    //
    // tun 内核现在自己监听 127.0.0.1:53（见 ConfigBuilder 里的 dns-in 入站），
    // 所以这里探测的是 **127.0.0.1**，探通才把系统 DNS 切过去——这是真正意义上的
    // 接管：解析请求一定走本机那个 root 进程，再经隧道出去，不再依赖
    // "发往 172.19.0.2 会不会被路由劫持"这种实测不成立的假设。
    //
    // 探测与切换都**不能在 UI 线程做**：
    //   ProbeDNS 每次最多等 900ms × 4 次；ApplyTunDNS 要对每个网络服务跑一次
    //   networksetup（单次超时 15 秒，VLAN / 雷雳桥接 / 闲置服务都会出现）。
    //   原来这些全在 UI 线程同步跑，实测表现是"勾上 TUN 后界面卡住十几秒"。
    auto tries = std::make_shared<int>(0);
    auto probe = std::make_shared<std::function<void()>>();
    *probe = [this, tries, probe] {
        if (vpn_pid == 0) return; // tun 已经退了，别再动 DNS

        const auto running_pid = vpn_pid;
        runOnNewThread([this, running_pid, tries, probe] {
            if (!NekoGui_mac::ProbeDNS("127.0.0.1", 53)) {
                if (++(*tries) >= 4) {
                    runOnUiThread([] {
                        MW_show_log("[Warn][mac] tun 的 DNS 没应答，这次不接管系统 DNS（域名仍由路由器解析）");
                    });
                    return;
                }
                runOnUiThread([probe] { QTimer::singleShot(800, GetMainWindow(), [probe] { (*probe)(); }); });
                return;
            }
            if (GetMainWindow()->vpn_pid != running_pid) return; // 期间被停掉了
            NekoGui_mac::ApplyTunDNS(true, "127.0.0.1");
            runOnUiThread([] {
                MW_show_log("[mac] 系统 DNS 已接管到 127.0.0.1:53（tun 内核 → 隧道解析，无泄漏）");
            });
        });
    };
    QTimer::singleShot(1200, this, [probe] { (*probe)(); });
#endif
#endif
    return true;
}

// 真正去杀 tun 内核 + 还原 DNS 快照。**不碰 UI，不改成员变量**，可以安全地在
// worker 线程里跑。返回 true 表示内核已经收到 SIGINT 并退出（pkill/killall/taskkill 成功）。
static bool stop_tun_process_blocking(bool unconditional, qint64 core_pid, qint64 vpn_pid_in)
{
#if defined(Q_OS_MACOS)
    // DNS 快照还原要跑 N 次 networksetup（单次超时 15 秒），必须在后台线程做，
    // 否则某个网络服务卡住时界面直接假死。只做残留清理：早期试验版留下的
    // tun-dns-state.json 会被还原删除，没有就是空操作。
    (void) core_pid;
    Q_UNUSED(vpn_pid_in)
    NekoGui_mac::RecoverTunDNSIfNeeded();
#endif
#if defined(Q_OS_WIN)
    auto ret = WinCommander::runProcessElevated("taskkill", {"/IM", "rekobox_core.exe",
                                                             "/FI",
                                                             "PID ne " + Int2String(core_pid)});
    return ret == 0;
#elif defined(Q_OS_MACOS)
    // mac 的停止分两条路：
    //
    // 1. 首选：哨兵握手（P0-3）。tun 脚本以 root 起来后自己盯着 config/tun-stop-request，
    //    GUI 只要建个文件（不需要提权）它就 SIGTERM 内核。
    //    → 正常启停全程只输一次密码，与 Windows 对齐；用户点"取消"也无从取消，
    //      不会出现"密码框一取消就关不掉、utun 与路由全残留"的断网死局。
    // 2. 回退：哨兵不在（旧版脚本 / 被强杀 / 手工起的内核）才走 osascript 提权 pkill。
    //    这条路仍然会弹密码框，但已带 20 秒上限（原来无参 = 30 秒，密码框弹着时整窗口假死）。
    Q_UNUSED(unconditional)
    if (NekoGui_mac::RequestTunStopBySentinel()) {
        if (NekoGui_mac::WaitTunExit(12000)) {
            qDebug() << "[mac] tun stopped via sentinel (no extra password prompt)";
            return true;
        }
        qWarning() << "[mac] sentinel did not stop the core within 12s, falling back to osascript";
    }

    QProcess p;
    p.start("osascript", {"-e", QStringLiteral("do shell script \"%1\" with administrator privileges")
                                    .arg("pkill -2 -U 0 rekobox_core")});
    if (!p.waitForFinished(20000)) {
        // osascript 自己卡住（密码框一直没人输/系统弹框被吞）：杀掉它，按失败处理
        p.kill();
        p.waitForFinished(1000);
        return false;
    }
    return p.exitCode() == 0;
#else
    QProcess p;
    if (unconditional) {
        p.start("pkexec", {"killall", "-2", "rekobox_core"});
    } else {
        p.start("pkexec", {"pkill", "-2", "-P", Int2String(vpn_pid_in)});
    }
    if (!p.waitForFinished(20000)) {
        p.kill();
        p.waitForFinished(1000);
        return false;
    }
    return p.exitCode() == 0;
#endif
}

bool MainWindow::StopVPNProcess(bool unconditional) {
    if (!unconditional && vpn_pid == 0) return true;

    // 这条同步路径只服务两个场景：退出链路、疑难解答→重置。
    // 用户点勾选框走的是 StopVPNProcessAsync()，不在 UI 线程里等。
    // 这里仍然在调用线程同步执行，但已带 20 秒上限（原来是无参 = 30 秒且无上限）。
    const auto ok = stop_tun_process_blocking(unconditional, core_process->processId(), vpn_pid);

    if (!unconditional) {
        ok ? vpn_pid = 0 : MessageBoxWarning(tr("Error"), tr("Failed to stop Tun process"));
    } else if (ok) {
        // unconditional（疑难解答 → 重置）是"强制清理残留"：pid 也一并清掉，
        // 否则后面的 StartVPNProcess() 会认为"已经有一个在跑"而直接 return true，
        // 用户点了重置反而再也起不来 TUN。
        vpn_pid = 0;
        if (NekoGui::dataStore->spmode_vpn) {
            NekoGui::dataStore->spmode_vpn = false;
            refresh_status();
        }
    }
    return ok;
}

void MainWindow::StopVPNProcessAsync(bool unconditional, const std::function<void(bool)> &onDone) {
    if (!unconditional && vpn_pid == 0) {
        onDone(true);
        return;
    }

    const auto core_pid = core_process->processId();
    const auto pid_snapshot = vpn_pid;
    // worker 线程负责：DNS 快照还原 + 等 osascript/pkexec 返回，全程不阻塞 UI
    runOnNewThread([=] {
        const auto ok = stop_tun_process_blocking(unconditional, core_pid, pid_snapshot);
        runOnUiThread([=] {
            if (!unconditional && ok) vpn_pid = 0;
            onDone(ok);
        });
    });
}
