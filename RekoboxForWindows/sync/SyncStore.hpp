#pragma once

#include <QHash>
#include <QString>

// 客户端本地订阅状态，落在 config/sync/ 下：
//   state.json  服务器地址、账号、令牌、角色、上次拉取版本/时间
//   nodes.json  「服务端节点 id → 本地实体 sync_id + 上次应用的链接指纹」
//
// 令牌是永久令牌（服务端不过期），只有「注销」「改口令」「删号」「禁用」会让它失效。
// 明文存放在 state.json：普通桌面程序的通行做法，若磁盘会被他人接触请开 BitLocker。
//
// 注意：服务端是只读节点池，客户端没有上传通道，所以这里不保存任何待推送的状态。

namespace NekoGui_sync {

    // 一条云端节点在本机的落地记录。
    struct ServerNodeRef {
        QString syncId;    // 本地实体上的 sync_id，固定为 "srv:<server id>"
        QString linkHash;  // 上次应用时的链接指纹；变了才覆盖本地
        qint64 updatedAt = 0;
    };

    class SyncStore {
    public:
        void load();

        void saveState() const;

        void saveNodes() const;

        [[nodiscard]] bool loggedIn() const { return !token.isEmpty() && !server.isEmpty() && !username.isEmpty(); }

        [[nodiscard]] bool isAdmin() const { return role == "admin"; }

        // 注销：只清会话与落地记录，保留服务器地址与用户名方便下次登录
        void clearSession();

        // state
        QString server;   // 例如 http://1.2.3.4:8080，不含结尾斜杠
        QString username;
        QString token;
        QString role;     // admin / user
        QString deviceId;
        QString deviceName;
        bool autoPull = true;       // 启动与定时自动拉取
        int pullIntervalMin = 30;   // 定时拉取间隔（分钟）
        bool useProxy = false;      // 服务端在墙内时一般不需要走代理
        qint64 lastVersion = 0;     // 上次拉到的节点池版本
        qint64 lastSyncAt = 0;
        int lastCount = 0;          // 上次拉到的节点数
        QString lastError;

        // nodes
        QHash<QString, ServerNodeRef> nodes; // key = 服务端节点 id
    };

} // namespace NekoGui_sync
