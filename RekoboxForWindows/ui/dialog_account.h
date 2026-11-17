#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// 账号与节点订阅：登录 / 注销 / 立即拉取 / 自动拉取开关。
// 界面全部用代码搭建（不走 .ui），减少一处需要手工维护的 XML。
//
// 模型是只读的：节点池由管理员在网页控制台维护，客户端只能拉取。

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
    QCheckBox *chkAutoPull = nullptr;
    QComboBox *comboInterval = nullptr;
    QCheckBox *chkUseProxy = nullptr;
    QLabel *labelAccount = nullptr;
    QLabel *labelStatus = nullptr;
    QLabel *labelCount = nullptr;
    QLabel *labelHint = nullptr;
};
