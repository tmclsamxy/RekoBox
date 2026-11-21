#include "dialog_vpn_settings.h"
#include "ui_dialog_vpn_settings.h"

#include "main/GuiUtils.hpp"
#include "main/NekoGui.hpp"
#include "ui/mainwindow_interface.h"

#include <QMessageBox>
#include <QLabel>
#include <QLayout>

DialogVPNSettings::DialogVPNSettings(QWidget *parent) : QDialog(parent), ui(new Ui::DialogVPNSettings) {
    ui->setupUi(this);
    ADD_ASTERISK(this);

    ui->fake_dns->setChecked(NekoGui::dataStore->fake_dns);
    ui->vpn_implementation->setCurrentIndex(NekoGui::dataStore->vpn_implementation);
    ui->vpn_mtu->setCurrentText(Int2String(NekoGui::dataStore->vpn_mtu));
    ui->vpn_ipv6->setChecked(NekoGui::dataStore->vpn_ipv6);
    ui->hide_console->setChecked(NekoGui::dataStore->vpn_hide_console);
#ifndef Q_OS_WIN
    ui->hide_console->setVisible(false);
#endif
    ui->strict_route->setChecked(NekoGui::dataStore->vpn_strict_route);
    ui->single_core->setChecked(NekoGui::dataStore->vpn_internal_tun);
#ifdef Q_OS_MACOS
    // mac 上这条选项永远不可能生效（主程序不是 root，建不了 utun），
    // 露出来只会让人勾了之后收到 "operation not permitted"，直接隐藏
    ui->single_core->setVisible(false);

    // P1-9：「按进程分流」在 mac 上是**空转**。
    // 这一版 sing-tun 在 darwin 不提供 ProcessInfo，process_name 规则既不报错也不生效
    // （HANDOFF §4.1 已实测记录）。让用户填了还以为有用，比不给入口更糟——
    // 排障时会被这条误导："我明明绕过了它"。
    // 所以禁用输入并写明原因，不要静默保留一个假开关。
    ui->vpn_rule_process->setEnabled(false);
    ui->vpn_rule_process->setPlaceholderText(
            tr("Not supported on macOS: the sing-tun build for darwin does not expose "
               "per-process information, so process-based routing cannot work here.\n"
               "Use \"Bypass CIDR\" instead, or per-app rules on the Android client."));
    ui->gb_process_name->setToolTip(
            tr("macOS limitation: process-based split routing is unavailable in this build."));
    auto mac_note = new QLabel(tr("⚠️ Not available on macOS (sing-tun on darwin has no ProcessInfo). "
                                  "Use Bypass CIDR instead."));
    mac_note->setWordWrap(true);
    mac_note->setStyleSheet("color: #b06000;");
    ui->gb_process_name->layout()->addWidget(mac_note);
#endif
    //
    D_LOAD_STRING_PLAIN(vpn_rule_cidr)
    D_LOAD_STRING_PLAIN(vpn_rule_process)
    //
    connect(ui->whitelist_mode, &QCheckBox::stateChanged, this, [=](int state) {
        if (state == Qt::Checked) {
            ui->gb_cidr->setTitle(tr("Proxy CIDR"));
            ui->gb_process_name->setTitle(tr("Proxy Process Name"));
        } else {
            ui->gb_cidr->setTitle(tr("Bypass CIDR"));
            ui->gb_process_name->setTitle(tr("Bypass Process Name"));
        }
    });
    ui->whitelist_mode->setChecked(NekoGui::dataStore->vpn_rule_white);
}

DialogVPNSettings::~DialogVPNSettings() {
    delete ui;
}

void DialogVPNSettings::accept() {
    //
    auto mtu = ui->vpn_mtu->currentText().toInt();
    if (mtu > 10000 || mtu < 1000) mtu = 9000;
    NekoGui::dataStore->vpn_implementation = ui->vpn_implementation->currentIndex();
    NekoGui::dataStore->fake_dns = ui->fake_dns->isChecked();
    NekoGui::dataStore->vpn_mtu = mtu;
    NekoGui::dataStore->vpn_ipv6 = ui->vpn_ipv6->isChecked();
    NekoGui::dataStore->vpn_hide_console = ui->hide_console->isChecked();
    NekoGui::dataStore->vpn_strict_route = ui->strict_route->isChecked();
    NekoGui::dataStore->vpn_rule_white = ui->whitelist_mode->isChecked();
    bool isInternalChanged = NekoGui::dataStore->vpn_internal_tun != ui->single_core->isChecked();
    NekoGui::dataStore->vpn_internal_tun = ui->single_core->isChecked();
    //
    D_SAVE_STRING_PLAIN(vpn_rule_cidr)
    D_SAVE_STRING_PLAIN(vpn_rule_process)
    //
    QStringList msg{"UpdateDataStore"};
    if (isInternalChanged) {
        msg << "NeedRestart";
    } else {
        msg << "VPNChanged";
    }
    MW_dialog_message("", msg.join(","));
    QDialog::accept();
}

void DialogVPNSettings::on_troubleshooting_clicked() {
    auto r = QMessageBox::information(this, tr("Troubleshooting"),
                                      tr("If you have trouble starting VPN, you can force reset rekobox_core process here.\n\n"
                                         "If still not working, see documentation for more information.\n"
                                         "https://matsuridayo.github.io/n-configuration/#vpn-tun"),
                                      tr("Reset"), tr("Cancel"), "",
                                      1, 1);
    if (r == 0) {
        GetMainWindow()->StopVPNProcess(true);
    }
}
