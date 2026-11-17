#include "SyncStore.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

namespace NekoGui_sync {

    namespace {
        constexpr auto kStateFile = "sync/state.json";
        constexpr auto kNodesFile = "sync/nodes.json";

        QJsonObject ReadObject(const QString &path) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return {};
            const auto doc = QJsonDocument::fromJson(f.readAll());
            f.close();
            return doc.object();
        }

        bool WriteObject(const QString &path, const QJsonObject &obj) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            // 先写临时文件再改名：过程中断电不会留下半截 JSON
            const auto tmp = path + ".tmp";
            {
                QFile f(tmp);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
                f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
                f.flush();
                f.close();
            }
            QFile::remove(path);
            return QFile::rename(tmp, path);
        }
    } // namespace

    void SyncStore::load() {
        const auto s = ReadObject(kStateFile);
        server = s.value("server").toString();
        username = s.value("username").toString();
        token = s.value("token").toString();
        role = s.value("role").toString();
        deviceId = s.value("device_id").toString();
        deviceName = s.value("device_name").toString();
        autoPull = s.value("auto_pull").toBool(true);
        pullIntervalMin = s.value("pull_interval").toInt(30);
        if (pullIntervalMin < 1) pullIntervalMin = 30;
        useProxy = s.value("use_proxy").toBool(false);
        lastVersion = static_cast<qint64>(s.value("last_version").toDouble());
        lastSyncAt = static_cast<qint64>(s.value("last_sync_at").toDouble());
        lastCount = s.value("last_count").toInt();
        lastError = s.value("last_error").toString();

        if (deviceId.isEmpty()) deviceId = QString::fromLatin1(QSysInfo::machineUniqueId());

        nodes.clear();
        const auto n = ReadObject(kNodesFile).value("nodes").toObject();
        for (const auto &k: n.keys()) {
            const auto o = n.value(k).toObject();
            ServerNodeRef ref;
            ref.syncId = o.value("sync_id").toString();
            ref.linkHash = o.value("link_hash").toString();
            ref.updatedAt = static_cast<qint64>(o.value("updated_at").toDouble());
            if (!ref.syncId.isEmpty()) nodes.insert(k, ref);
        }
    }

    void SyncStore::saveState() const {
        QJsonObject o;
        o.insert("server", server);
        o.insert("username", username);
        o.insert("token", token);
        o.insert("role", role);
        o.insert("device_id", deviceId);
        o.insert("device_name", deviceName);
        o.insert("auto_pull", autoPull);
        o.insert("pull_interval", pullIntervalMin);
        o.insert("use_proxy", useProxy);
        o.insert("last_version", static_cast<double>(lastVersion));
        o.insert("last_sync_at", static_cast<double>(lastSyncAt));
        o.insert("last_count", lastCount);
        o.insert("last_error", lastError);
        WriteObject(kStateFile, o);
    }

    void SyncStore::saveNodes() const {
        QJsonObject items;
        for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it) {
            QJsonObject e;
            e.insert("sync_id", it.value().syncId);
            e.insert("link_hash", it.value().linkHash);
            e.insert("updated_at", static_cast<double>(it.value().updatedAt));
            items.insert(it.key(), e);
        }
        QJsonObject o;
        o.insert("nodes", items);
        WriteObject(kNodesFile, o);
    }

    void SyncStore::clearSession() {
        token.clear();
        role.clear();
        lastVersion = 0;
        lastSyncAt = 0;
        lastCount = 0;
        lastError.clear();
        // 落地记录一并清掉：重新登录时按服务端现状重建，不会留下孤儿
        nodes.clear();
        saveState();
        saveNodes();
    }

} // namespace NekoGui_sync
