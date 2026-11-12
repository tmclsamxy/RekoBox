#include "SyncModel.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>

namespace NekoGui_sync {

    namespace {
        // 报文里的固定字段名，集中一处便于以后升级 schema
        constexpr auto kSchema = "schema";
        constexpr auto kExportedAt = "exported_at";
        constexpr auto kGroups = "groups";
        constexpr auto kProfiles = "profiles";
        constexpr auto kId = "id";
        constexpr auto kMtime = "mtime";
        constexpr auto kDeleted = "deleted";
        constexpr auto kData = "data";
        constexpr auto kGroup = "group";

        SyncItem ItemFromJson(const QString &kind, const QJsonObject &o) {
            SyncItem item;
            item.kind = kind;
            item.id = o.value(kId).toString();
            item.groupId = o.value(kGroup).toString();
            item.mtime = static_cast<qint64>(o.value(kMtime).toDouble());
            item.deleted = o.value(kDeleted).toBool();
            item.data = o.value(kData).toObject();
            return item;
        }

        QJsonObject ItemToJson(const SyncItem &item) {
            QJsonObject o;
            o.insert(kId, item.id);
            o.insert(kMtime, static_cast<double>(item.mtime));
            if (item.deleted) o.insert(kDeleted, true);
            if (!item.groupId.isEmpty()) o.insert(kGroup, item.groupId);
            if (!item.deleted) o.insert(kData, item.data);
            return o;
        }

        void MergeList(const QList<SyncItem> &local, const QList<SyncItem> &remote,
                       QList<SyncItem> *out, int *conflicts) {
            QMap<QString, SyncItem> map;
            for (const auto &item: local) {
                if (!item.id.isEmpty()) map.insert(item.id, item);
            }
            for (const auto &r: remote) {
                if (r.id.isEmpty()) continue;
                if (!map.contains(r.id)) {
                    map.insert(r.id, r);
                    continue;
                }
                auto &l = map[r.id];
                if (l.sameContent(r)) {
                    // 内容一致，只是时间戳不同：取较大值，避免下次又被判成「新改动」
                    if (r.mtime > l.mtime) l.mtime = r.mtime;
                    continue;
                }
                if (!l.deleted && !r.deleted && conflicts != nullptr) (*conflicts)++;
                if (l.mtime == r.mtime && l.deleted != r.deleted) {
                    // 同一时刻的「删除 vs 编辑」分歧：删除优先。
                    // 否则「正在使用的节点被远端删除」这条会每轮复活，永远收敛不了。
                    if (r.deleted) l = r;
                    continue;
                }
                // 远端严格更新才覆盖；相等或更旧时保留本地，保证同一输入多次合并结果不变
                if (r.mtime > l.mtime) l = r;
            }
            // QMap 已按 id 排序，输出稳定，便于比较与排查
            out->clear();
            for (const auto &item: map) out->append(item);
        }
    } // namespace

    bool SyncItem::sameContent(const SyncItem &other) const {
        if (deleted != other.deleted) return false;
        if (deleted) return true; // 墓碑只比「谁删的、什么时候删的」，由 mtime 决定
        if (groupId != other.groupId) return false;
        return ItemHash(data) == ItemHash(other.data);
    }

    const SyncItem *Vault::find(const QString &kind, const QString &id) const {
        const auto &list = kind == "group" ? groups : profiles;
        for (const auto &item: list) {
            if (item.id == id) return &item;
        }
        return nullptr;
    }

    int Vault::liveCount() const {
        int n = 0;
        for (const auto &g: groups) {
            if (!g.deleted) n++;
        }
        for (const auto &p: profiles) {
            if (!p.deleted) n++;
        }
        return n;
    }

    Vault VaultFromJson(const QJsonObject &obj) {
        Vault v;
        for (const auto &it: obj.value(kGroups).toArray()) {
            auto item = ItemFromJson("group", it.toObject());
            if (!item.id.isEmpty()) v.groups.append(item);
        }
        for (const auto &it: obj.value(kProfiles).toArray()) {
            auto item = ItemFromJson("profile", it.toObject());
            if (!item.id.isEmpty()) v.profiles.append(item);
        }
        return v;
    }

    QJsonObject VaultToJson(const Vault &vault) {
        QJsonArray groups;
        for (const auto &item: vault.groups) groups.append(ItemToJson(item));
        QJsonArray profiles;
        for (const auto &item: vault.profiles) profiles.append(ItemToJson(item));

        QJsonObject o;
        o.insert(kSchema, 1);
        o.insert(kExportedAt, static_cast<double>(NowMs()));
        o.insert(kGroups, groups);
        o.insert(kProfiles, profiles);
        return o;
    }

    Vault MergeVault(const Vault &local, const Vault &remote, int *conflicts) {
        Vault merged;
        MergeList(local.groups, remote.groups, &merged.groups, conflicts);
        MergeList(local.profiles, remote.profiles, &merged.profiles, conflicts);
        return merged;
    }

    bool VaultEquals(const Vault &a, const Vault &b) {
        if (a.groups.size() != b.groups.size() || a.profiles.size() != b.profiles.size()) return false;
        auto same = [](const QList<SyncItem> &x, const QList<SyncItem> &y) {
            for (int i = 0; i < x.size(); i++) {
                if (x[i].id != y[i].id) return false;
                if (x[i].mtime != y[i].mtime) return false;
                if (!x[i].sameContent(y[i])) return false;
            }
            return true;
        };
        return same(a.groups, b.groups) && same(a.profiles, b.profiles);
    }

    QString ItemHash(const QJsonObject &data) {
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    }

    QString NewSyncId() {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    qint64 NowMs() {
        return QDateTime::currentMSecsSinceEpoch();
    }

} // namespace NekoGui_sync
