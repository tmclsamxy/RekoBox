#pragma once

#include <QJsonArray>
#include <QObject>
#include <QTimer>

#include "SyncApi.hpp"
#include "SyncStore.hpp"

// 订阅引擎：登录后从服务端**只读**拉取节点池，落到本地分组。
//
// 模型（v2）：
//   服务端由管理员维护一份共享节点池，客户端没有上传通道。
//   一次拉取 = GET /api/v1/nodes，按节点 id 对应到本地实体（sync_id = "srv:<id>"）。
//   · 服务端新增 → 本地新建
//   · 服务端链接变了 → 覆盖本地这一条（本地改名/调参不会被无谓覆盖）
//   · 服务端删除/禁用 → 本地删除（正在使用的节点等停止后再删）
//
// 生命周期：整个进程一个实例，随主窗口初始化，常驻后台。
// 离线策略：网络失败退避重试（5s → 10s → … → 5min 封顶），本地配置照常可用。

namespace NekoGui_sync {

    struct PullStats {
        int created = 0;
        int updated = 0;
        int removed = 0;
        int skipped = 0; // 链接解析失败 / 类型不支持

        [[nodiscard]] bool changed() const { return created + updated + removed > 0; }
    };

    class SyncEngine : public QObject {
        Q_OBJECT

    public:
        enum class State {
            LoggedOut, // 未登录：纯本地模式
            Idle,      // 已登录，空闲
            Pulling,
            Offline,   // 网络不通，等待重试
            Error,
        };

        static SyncEngine *instance();

        // 主窗口构造后调用一次
        void init();

        [[nodiscard]] SyncStore *store() { return &state; }

        [[nodiscard]] bool loggedIn() const { return state.loggedIn(); }

        [[nodiscard]] bool syncing() const { return pulling_; }

        [[nodiscard]] State stateNow() const { return current; }

        [[nodiscard]] QString statusText() const;

        // 登录（服务器地址可带可不带 http://；不带协议按 http 处理）
        void login(const QString &server, const QString &username, const QString &password);

        // 注销：吊销服务端令牌并清掉本地会话与落地记录
        void logout();

        // 立即拉取；reason 只用于日志
        void syncNow(const QString &reason);

        // 只读模式：本地改动不再上传。保留空实现供 db 层回调调用。
        void noteLocalChange();

        void persistState();

        // 改过自动拉取开关/间隔后重新排定时器
        void restartTimer();

    signals:

        void stateChanged();

        void loginFinished(bool ok, const QString &message);

        void syncFinished(bool ok, const QString &message);

    private:
        explicit SyncEngine(QObject *parent = nullptr);

        void prepareApi();

        void runPull(int attempt);

        // 把服务端节点池应用到本地，返回统计
        PullStats applyNodes(qint64 version, const QJsonArray &nodes);

        // 取（必要时新建）云端分组，返回本地 gid
        int ensureGroup(const QString &name);

        void finishOk(const PullStats &stats, bool unchanged);

        void finishFail(const ApiResult &r);

        void scheduleRetry();

        void setState(State s, const QString &error = QString());

        SyncStore state;
        SyncApi *api = nullptr;

        QTimer retryTimer;
        QTimer pullTimer;

        bool inited = false;
        bool pulling_ = false;
        bool queued_ = false;
        bool applying_ = false;
        int retryDelayMs = 5000;
        State current = State::LoggedOut;
    };

    // 轻量通知入口：Database.cpp 只依赖这一个符号，避免循环包含
    void NotifyLocalChange();

} // namespace NekoGui_sync
