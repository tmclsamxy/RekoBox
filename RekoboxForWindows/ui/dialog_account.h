#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

// 账号与同步：登录 / 注销 / 立即同步 / 自动同步开关。
// 界面全部用代码搭建（不走 .ui），减少一处需要手工维护的 XML。

class DialogAccount : public QDialog {
    Q_OBJECT

public:
    explicit DialogAccount(QWidget *parent = nullptr);

private slots:

    void onLoginClicked();

    void onLogoutClicked();

    void onSyncClicked();

    void refresh();

private:
    void setBusy(bool busy);

    QLineEdit *editServer = nullptr;
    QLineEdit *editUser = nullptr;
    QLineEdit *editPassword = nullptr;
    QPushButton *btnLogin = nullptr;
    QPushButton *btnLogout = nullptr;
    QPushButton *btnSync = nullptr;
    QCheckBox *chkAutoSync = nullptr;
    QCheckBox *chkUseProxy = nullptr;
    QLabel *labelAccount = nullptr;
    QLabel *labelStatus = nullptr;
    QLabel *labelHint = nullptr;
};
