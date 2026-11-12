#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

// 同步数据模型：只做「节点集合 <-> JSON」与「逐条目合并」，不碰磁盘、不碰网络、不碰 UI。
// 纯函数，方便单独编译验证。

namespace NekoGui_sync {

    // 一个同步条目 = 一个分组或一个节点配置。
    //
    // 跨设备唯一标识用 UUID（落在实体 JSON 的 sync_id 字段），不能用本地 int id：
    // 两台设备各自新建节点时 int id 必然撞车，排序（-flag_reorder）还会把 id 整体重排。
    struct SyncItem {
        QString kind;     // "group" | "profile"
        QString id;       // sync_id
        QString groupId;  // 仅 profile：所属分组的 sync_id
        qint64 mtime = 0; // 最后修改时间（毫秒时间戳）
        bool deleted = false;
        QJsonObject data; // 实体内容，已剔除机器本地字段（id / gid / 延迟 / 流量）

        [[nodiscard]] bool sameContent(const SyncItem &other) const;
    };

    struct Vault {
        QList<SyncItem> groups;
        QList<SyncItem> profiles;

        [[nodiscard]] bool isEmpty() const { return groups.isEmpty() && profiles.isEmpty(); }

        [[nodiscard]] const SyncItem *find(const QString &kind, const QString &id) const;

        [[nodiscard]] int liveCount() const;
    };

    Vault VaultFromJson(const QJsonObject &obj);

    QJsonObject VaultToJson(const Vault &vault);

    // 逐条目 last-write-wins 合并。规则见 docs/04-account-sync.md：
    //   1. 只有一边有 → 采用
    //   2. 两边都有且内容相同 → 保留较大的 mtime
    //   3. 两边都有且内容不同 → mtime 大的赢；相同则本地赢（保证幂等）
    //   4. deleted 条目与普通条目同等参与比较，所以「删除」能覆盖旧编辑，
    //      「新编辑」也能把被删条目救回来
    // conflicts 统计两边都改过且内容不同的条目数（仅用于提示）。
    Vault MergeVault(const Vault &local, const Vault &remote, int *conflicts = nullptr);

    // 内容是否等价（忽略 mtime）。用于判断「合并结果是否与远端一致」，一致就不必回传。
    [[nodiscard]] bool VaultEquals(const Vault &a, const Vault &b);

    [[nodiscard]] QString ItemHash(const QJsonObject &data);

    [[nodiscard]] QString NewSyncId();

    [[nodiscard]] qint64 NowMs();

} // namespace NekoGui_sync
