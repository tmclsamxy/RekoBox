#pragma once

#include <QObject>
#include <QTimer>

#include "SyncApi.hpp"
#include "SyncModel.hpp"
#include "SyncStore.hpp"

// 同步调度器：负责「什么时候同步、同步失败怎么办」，具体合并逻辑在 SyncModel。
//
// 生命周期：整个进程一个实例，随主窗口初始化，常驻后台。
// 离线策略：任何网络失败都退避重试（5s → 10s → … → 5min 封顶），
//           本地改动照常落盘，pending 标记持久化，重启后继续补推。

namespace NekoGui_sync {

    class SyncEngine : public QObject {
        Q_OBJECT

    public:
        enum class State {
            LoggedOut, // 未登录：纯本地模式
            Idle,      // 已登录，空闲
            Syncing,
            Offline,   // 网络不通，等待重试
            Error,
        };

        static SyncEngine *instance();

        // 主窗口构造后调用一次
        void init();

        [[nodiscard]] SyncStore *store() { return &state; }

        [[nodiscard]] bool loggedIn() const { return state.loggedIn(); }

        [[nodiscard]] bool syncing() const { return syncing_; }

        [[nodiscard]] State stateNow() const { return current; }

        [[nodiscard]] QString statusText() const;

        // 登录（服务器地址可带可不带 http://；不带协议按 http 处理）
        void login(const QString &server, const QString &username, const QString &password);

        // 注销：吊销服务端令牌并清掉本地会话与同步基线
        void logout();

        // 立即同步；reason 只用于日志
        void syncNow(const QString &reason);

        // 本地数据变动（由实体 Save 回调触发），做防抖后自动推送
        void noteLocalChange();

        void persistState();

    signals:

        void stateChanged();

        void loginFinished(bool ok, const QString &message);

        void syncFinished(bool ok, const QString &message);

    private:
        explicit SyncEngine(QObject *parent = nullptr);

        void prepareApi();

        [[nodiscard]] Vault buildLocalVault();

        void stampItem(SyncItem &item, bool firstBaseline);

        void applyVault(const Vault &vault);

        void recordApplied(const SyncItem &item);

        void runRound(int attempt);

        void finishOk(int conflicts, bool pushed);

        void finishFail(const ApiResult &r);

        void scheduleRetry();

        void setState(State s, const QString &error = "");

        SyncStore state;
        SyncApi *api = nullptr;

        QTimer debounce;
        QTimer retryTimer;

        bool inited = false;
        bool syncing_ = false;
        bool queued_ = false;
        bool applying_ = false;
        int retryDelayMs = 5000;
        // 全新设备（本机只有一个空分组）首次同步时，用服务端的默认分组替换它，
        // 避免每台新机器都往云端塞一个"默认"分组
        int adoptRemoteGroupId = -1;
        State current = State::LoggedOut;
    };

    // 轻量通知入口：Database.cpp 只依赖这一个符号，避免循环包含
    void NotifyLocalChange();

} // namespace NekoGui_sync
