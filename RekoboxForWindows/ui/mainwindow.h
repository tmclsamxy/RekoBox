#pragma once

#include <QMainWindow>

#include "main/NekoGui.hpp"

#ifndef MW_INTERFACE

#include <QTime>
#include <QTableWidgetItem>
#include <QKeyEvent>
#include <QSystemTrayIcon>
#include <QProcess>
#include <QTextDocument>
#include <QShortcut>
#include <QSemaphore>
#include <QMutex>
#include <QMenu>

#include "GroupSort.hpp"

#include "db/ProxyEntity.hpp"
#include "main/GuiUtils.hpp"

#endif

namespace NekoGui_sys {
    class CoreProcess;
}

QT_BEGIN_NAMESPACE
namespace Ui {
    class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    ~MainWindow() override;

    void refresh_proxy_list(const int &id = -1);

    void show_group(int gid);

    void refresh_groups();

    void refresh_status(const QString &traffic_update = "");

    void neko_start(int _id = -1);

    void neko_stop(bool crash = false, bool sem = false);

    void neko_set_spmode_system_proxy(bool enable, bool save = true);

    // sync=true 只用于退出链路：那里紧接着要读 spmode_vpn 判断能否继续退出，
    // 没法等异步回调。用户主动取消勾选一律走异步，避免 UI 线程被密码框卡死。
    void neko_set_spmode_vpn(bool enable, bool save = true, bool sync = false);

    void show_log_impl(const QString &log);

    void start_select_mode(QObject *context, const std::function<void(int)> &callback);

    void refresh_connection_list(const QJsonArray &arr);

    void RegisterHotkey(bool unregister);

    // 全局热键注册失败的说明文本（空 = 全部成功）。
    // 原来 QHotkey::isRegistered() 为 false 时直接 deleteLater 静默丢弃，
    // 用户按了没反应却看不到任何提示，只能以为"这个功能坏了"。
    QString HotkeyFailureSummary() const;

    // 停 TUN 内核有两条路：
    //   StopVPNProcess()           同步、带超时。只给退出链路和"疑难解答→重置"用——
    //                              那两处必须拿到结果才能决定下一步。
    //   StopVPNProcessAsync()      异步，在 worker 线程里等 osascript/pkexec，
    //                              用户勾选取消时 UI 不会冻结（原来无参
    //                              waitForFinished = 30 秒，密码框还弹着的时候整窗口假死）。
    // 两者都返回"内核是否真的停掉了"。false 时调用方必须保持 spmode_vpn = true 并提示，
    // 否则会出现"界面显示已关、utun 与 72 条路由还在"→ 整机断网。
    bool StopVPNProcess(bool unconditional = false);

    void StopVPNProcessAsync(bool unconditional, const std::function<void(bool)> &onDone);

signals:

    void profile_selected(int id);

public slots:

    void on_commitDataRequest();

    void on_menu_exit_triggered();

#ifndef MW_INTERFACE

private slots:

    void on_masterLogBrowser_customContextMenuRequested(const QPoint &pos);

    void on_menu_basic_settings_triggered();

    void on_menu_routing_settings_triggered();

    void on_menu_vpn_settings_triggered();

    void on_menu_hotkey_settings_triggered();

    void on_menu_account_triggered();

    void on_menu_add_from_input_triggered();

    void on_menu_add_from_clipboard_triggered();

    void on_menu_clone_triggered();

    void on_menu_move_triggered();

    void on_menu_delete_triggered();

    void on_menu_reset_traffic_triggered();

    void on_menu_profile_debug_info_triggered();

    void on_menu_copy_links_triggered();

    void on_menu_copy_links_nkr_triggered();

    void on_menu_export_config_triggered();

    void display_qr_link(bool nkrFormat = false);

    void on_menu_scan_qr_triggered();

    void on_menu_clear_test_result_triggered();

    void on_menu_manage_groups_triggered();

    void on_menu_select_all_triggered();

    void on_menu_delete_repeat_triggered();

    void on_menu_remove_unavailable_triggered();

    void on_menu_update_subscription_triggered();

    void on_menu_resolve_domain_triggered();

    void on_proxyListTable_itemDoubleClicked(QTableWidgetItem *item);

    void on_proxyListTable_customContextMenuRequested(const QPoint &pos);

    void on_tabWidget_currentChanged(int index);

private:
    Ui::MainWindow *ui;
    QSystemTrayIcon *tray;
#ifdef Q_OS_MACOS
    // mac 上托盘用独立菜单：菜单栏（原生 NSMenu）已经复用了 ui->menu_program，
    // 再让托盘 setContextMenu(ui->menu_program) 会让两者互相破坏。
    QMenu *tray_menu = nullptr;
#endif
    QShortcut *shortcut_ctrl_f = new QShortcut(QKeySequence("Ctrl+F"), this);
    QShortcut *shortcut_esc = new QShortcut(QKeySequence("Esc"), this);
    //
    NekoGui_sys::CoreProcess *core_process;
    qint64 vpn_pid = 0;
    // 停 TUN 是否正在进行。异步停 TUN 期间用户重复点勾选框，只提示不重复提权。
    bool vpn_stopping = false;
    //
    bool qvLogAutoScoll = true;
    QTextDocument *qvLogDocument = new QTextDocument(this);
    //
    QString title_error;
    int icon_status = -1;
    std::shared_ptr<NekoGui::ProxyEntity> running;
    QString traffic_update_cache;
    QTime last_test_time;
    //
    int proxy_last_order = -1;
    bool select_mode = false;
    QMutex mu_starting;
    QMutex mu_stopping;
    QMutex mu_exit;
    QSemaphore sem_stopped;
    int exit_reason = 0;
    // 上一次 RegisterHotkey 里注册失败的热键（"描述 = 组合键"），供对话框提示
    QStringList hotkey_failures;

    QList<std::shared_ptr<NekoGui::ProxyEntity>> get_now_selected_list();

    QList<std::shared_ptr<NekoGui::ProxyEntity>> get_selected_or_group();

    void dialog_message_impl(const QString &sender, const QString &info);

    void refresh_proxy_list_impl(const int &id = -1, GroupSortAction groupSortAction = {});

    void refresh_proxy_list_impl_refresh_data(const int &id = -1);

    void keyPressEvent(QKeyEvent *event) override;

    void closeEvent(QCloseEvent *event) override;

#ifdef Q_OS_MACOS
    // 托盘模式启动时隐藏了 Dock 图标（Accessory）。任何方式把窗口显示出来
    // （菜单栏"显示窗口"、热键、二次启动唤起）都要把它切回 Regular，
    // 否则窗口会浮在很低的层级、Cmd+Tab 也找不到。
    void showEvent(QShowEvent *event) override;
#endif

    //

    void HotkeyEvent(const QString &key);

    bool StartVPNProcess();

    // grpc and ...

    static void setup_grpc();

    void speedtest_current_group(int mode, bool test_group);

    void speedtest_current();

    static void stop_core_daemon();


protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

#endif // MW_INTERFACE
};

inline MainWindow *GetMainWindow() {
    return (MainWindow *) mainwindow;
}

void UI_InitMainWindow();
