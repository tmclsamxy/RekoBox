#pragma once

#include <QHash>
#include <QList>
#include <QString>

// 客户端本地同步状态，落在 config/sync/ 下：
//   state.json  服务器地址、账号、令牌、设备名、上次同步版本/时间
//   meta.json   每个条目的内容指纹与 mtime、待同步的删除墓碑
//
// 令牌是永久令牌（服务端不过期），只有「注销」「改口令」「删号」会让它失效。
// 明文存放在 state.json：普通桌面程序的通行做法，若磁盘会被他人接触，
// 请开启 BitLocker/Veracrypt。文档里有说明。

namespace NekoGui_sync {

    struct SyncMetaEntry {
        QString hash;    // 内容指纹；空 = 「内容刚被远端覆盖，下次扫描直接采信」
        qint64 mtime = 0;
    };

    struct SyncTombstone {
        QString kind;
        QString id;
        qint64 mtime = 0;
    };

    class SyncStore {
    public:
        void load();

        void saveState() const;

        void saveMeta() const;

        [[nodiscard]] bool loggedIn() const { return !token.isEmpty() && !server.isEmpty() && !username.isEmpty(); }

        // 注销：只清会话与基线，保留服务器地址与用户名方便下次登录
        void clearSession();

        [[nodiscard]] QString path(const QString &suffix) const { return server + suffix; }

        static QString key(const QString &kind, const QString &id) { return kind + ":" + id; }

        // state
        QString server;      // 例如 http://1.2.3.4:8080，不含结尾斜杠
        QString username;
        QString token;
        QString deviceId;
        QString deviceName;
        bool autoSync = true;
        bool useProxy = false;   // 服务端在墙内时一般不需要走代理
        bool pending = false;    // 有本地改动尚未推送到服务端
        qint64 lastVersion = 0;
        qint64 lastSyncAt = 0;
        QString lastSyncDevice;
        QString lastError;

        // meta
        QHash<QString, SyncMetaEntry> items;
        QList<SyncTombstone> tombstones;
    };

} // namespace NekoGui_sync
