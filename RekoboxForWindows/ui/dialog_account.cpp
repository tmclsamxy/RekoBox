#include "dialog_account.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <memory>

#include "main/NekoGui.hpp"
#include "sync/SyncEngine.hpp"

DialogAccount::DialogAccount(QWidget *parent) : QDialog(parent) {
    auto engine = NekoGui_sync::SyncEngine::instance();
    auto store = engine->store();

    setWindowTitle(tr("账号与节点订阅"));
    setMinimumWidth(520);

    editServer = new QLineEdit(this);
    editServer->setPlaceholderText(QStringLiteral("http://1.2.3.4:8080"));
    editServer->setText(store->server);
    editServer->setToolTip(tr("自建服务地址。只填 IP:端口 时按 http 处理。"));

    editUser = new QLineEdit(this);
    editUser->setText(store->username);
    editUser->setPlaceholderText(tr("服务端管理员分配的账号"));

    editPassword = new QLineEdit(this);
    editPassword->setEchoMode(QLineEdit::Password);
    editPassword->setPlaceholderText(tr("口令只用于登录，不会上传"));

    auto form = new QFormLayout;
    form->addRow(tr("服务器地址"), editServer);
    form->addRow(tr("用户名"), editUser);
    form->addRow(tr("口令"), editPassword);

    btnLogin = new QPushButton(tr("登录"), this);
    btnLogin->setDefault(true);
    btnLogout = new QPushButton(tr("注销"), this);
    btnSync = new QPushButton(tr("立即拉取"), this);

    auto buttons = new QHBoxLayout;
    buttons->addWidget(btnLogin);
    buttons->addStretch();
    buttons->addWidget(btnSync);
    buttons->addWidget(btnLogout);

    auto line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);

    labelAccount = new QLabel(this);
    labelAccount->setWordWrap(true);
    labelStatus = new QLabel(this);
    labelStatus->setWordWrap(true);
    labelCount = new QLabel(this);
    labelCount->setWordWrap(true);
    labelCount->setStyleSheet("color: palette(mid);");

    chkAutoPull = new QCheckBox(tr("自动拉取节点"), this);
    chkAutoPull->setChecked(store->autoPull);
    comboInterval = new QComboBox(this);
    comboInterval->addItem(tr("每 15 分钟"), 15);
    comboInterval->addItem(tr("每 30 分钟"), 30);
    comboInterval->addItem(tr("每 1 小时"), 60);
    comboInterval->addItem(tr("每 3 小时"), 180);
    comboInterval->addItem(tr("每 12 小时"), 720);
    int idx = comboInterval->findData(store->pullIntervalMin);
    comboInterval->setCurrentIndex(idx >= 0 ? idx : 1);
    auto pullRow = new QHBoxLayout;
    pullRow->addWidget(chkAutoPull);
    pullRow->addWidget(comboInterval);
    pullRow->addStretch();

    chkUseProxy = new QCheckBox(tr("经本地代理拉取"), this);
    chkUseProxy->setChecked(store->useProxy);
    chkUseProxy->setToolTip(tr("服务端在境内时一般不需要；服务端在境外或本机 DNS 被污染时可勾选。"));

    labelHint = new QLabel(this);
    labelHint->setWordWrap(true);
    labelHint->setStyleSheet("color: palette(mid);");
    labelHint->setText(tr("节点由管理员在网页控制台统一维护，客户端<b>只能拉取，不能上传</b>，"
                          "所以本机增删改不会影响其他用户。\n"
                          "未登录时全部功能照常，只是不会自动更新节点。"
                          "登录后令牌长期有效，只有主动注销、服务端改口令或删号才会失效。"));

    auto layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(line);
    layout->addWidget(labelAccount);
    layout->addWidget(labelStatus);
    layout->addWidget(labelCount);
    layout->addLayout(pullRow);
    layout->addWidget(chkUseProxy);
    layout->addWidget(labelHint);
    layout->setSizeConstraint(QLayout::SetMinimumSize);

    connect(btnLogin, &QPushButton::clicked, this, &DialogAccount::onLoginClicked);
    connect(btnLogout, &QPushButton::clicked, this, &DialogAccount::onLogoutClicked);
    connect(btnSync, &QPushButton::clicked, this, &DialogAccount::onSyncClicked);
    connect(editPassword, &QLineEdit::returnPressed, this, &DialogAccount::onLoginClicked);
    connect(chkAutoPull, &QCheckBox::toggled, this, [store, engine](bool on) {
        store->autoPull = on;
        store->saveState();
        engine->restartTimer();
    });
    connect(comboInterval, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [store, engine, this](int) {
                store->pullIntervalMin = comboInterval->currentData().toInt();
                store->saveState();
                engine->restartTimer();
            });
    connect(chkUseProxy, &QCheckBox::toggled, this, [store](bool on) {
        store->useProxy = on;
        store->saveState();
    });

    connect(engine, &NekoGui_sync::SyncEngine::stateChanged, this, &DialogAccount::refresh);
    connect(engine, &NekoGui_sync::SyncEngine::syncFinished, this,
            [this](bool ok, const QString &msg) {
                if (!ok) QMessageBox::warning(this, tr("拉取节点"), msg);
                refresh();
            });

    refresh();
}

void DialogAccount::setBusy(bool busy) {
    btnLogin->setEnabled(!busy);
    btnLogout->setEnabled(!busy);
    btnSync->setEnabled(!busy);
    editServer->setEnabled(!busy);
    editUser->setEnabled(!busy);
    editPassword->setEnabled(!busy);
}

void DialogAccount::refresh() {
    auto engine = NekoGui_sync::SyncEngine::instance();
    auto store = engine->store();
    const bool logged = engine->loggedIn();

    labelAccount->setText(logged
                              ? tr("当前账号：%1 @ %2").arg(store->username, store->server)
                              : tr("当前状态：未登录（本地模式）"));
    labelStatus->setText(engine->statusText());
    if (store->lastSyncAt > 0) {
        labelCount->setText(tr("上次拉取：%1 · %2 个节点")
                                .arg(QDateTime::fromMSecsSinceEpoch(store->lastSyncAt)
                                         .toString("yyyy-MM-dd HH:mm:ss"))
                                .arg(store->lastCount));
    } else {
        labelCount->setText(tr("还没有拉取过节点"));
    }

    btnLogin->setVisible(!logged);
    editPassword->setVisible(!logged);
    btnLogout->setEnabled(logged);
    btnSync->setEnabled(logged && !engine->syncing());
    chkAutoPull->setEnabled(logged);
    comboInterval->setEnabled(logged && store->autoPull);
    chkUseProxy->setEnabled(logged);
    setBusy(false);
}

void DialogAccount::onLoginClicked() {
    auto engine = NekoGui_sync::SyncEngine::instance();
    setBusy(true);
    labelStatus->setText(tr("正在登录…"));

    // loginFinished 与 syncFinished 都可能到；这里只关心登录这一步
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(engine, &NekoGui_sync::SyncEngine::loginFinished, this,
                    [this, conn](bool ok, const QString &msg) {
                        QObject::disconnect(*conn);
                        setBusy(false);
                        if (!ok) {
                            labelStatus->setText(msg);
                            QMessageBox::warning(this, tr("登录"), msg);
                        }
                        refresh();
                    });

    engine->login(editServer->text(), editUser->text(), editPassword->text());
    editPassword->clear();
}

void DialogAccount::onLogoutClicked() {
    auto engine = NekoGui_sync::SyncEngine::instance();
    if (QMessageBox::question(this, tr("注销"),
                              tr("注销后本机令牌失效，已拉取到本地的节点仍保留。\n"
                                 "重新登录会按服务端当前节点池重新同步。\n\n确定注销吗？"))
        != QMessageBox::Yes) {
        return;
    }
    setBusy(true);
    labelStatus->setText(tr("正在注销…"));
    engine->logout();
}

void DialogAccount::onSyncClicked() {
    auto engine = NekoGui_sync::SyncEngine::instance();
    btnSync->setEnabled(false);
    labelStatus->setText(tr("正在拉取…"));
    // 手动拉取一律强制全量：用户点这个按钮就是要"现在真的去拿一次"，
    // 带上本地缓存的版本号会被服务端 unchanged 短路，点了等于没点。
    engine->syncNow("manual", true);
}
