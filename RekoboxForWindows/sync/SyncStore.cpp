#include "SyncStore.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

namespace NekoGui_sync {

    namespace {
        constexpr auto kStateFile = "sync/state.json";
        constexpr auto kMetaFile = "sync/meta.json";

        QJsonObject ReadObject(const QString &path) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return {};
            const auto doc = QJsonDocument::fromJson(f.readAll());
            f.close();
            return doc.object();
        }

        bool WriteObject(const QString &path, const QJsonObject &obj) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            // 先写临时文件再改名：同步过程中断电不会留下半截 JSON
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
        deviceId = s.value("device_id").toString();
        deviceName = s.value("device_name").toString();
        autoSync = s.value("auto_sync").toBool(true);
        useProxy = s.value("use_proxy").toBool(false);
        pending = s.value("pending").toBool(false);
        lastVersion = static_cast<qint64>(s.value("last_version").toDouble());
        lastSyncAt = static_cast<qint64>(s.value("last_sync_at").toDouble());
        lastSyncDevice = s.value("last_sync_device").toString();
        lastError = s.value("last_error").toString();

        if (deviceId.isEmpty()) deviceId = QString::fromLatin1(QSysInfo::machineUniqueId());

        items.clear();
        tombstones.clear();
        const auto m = ReadObject(kMetaFile);
        const auto itemsObj = m.value("items").toObject();
        for (const auto &k: itemsObj.keys()) {
            const auto o = itemsObj.value(k).toObject();
            SyncMetaEntry e;
            e.hash = o.value("hash").toString();
            e.mtime = static_cast<qint64>(o.value("mtime").toDouble());
            items.insert(k, e);
        }
        for (const auto &v: m.value("tombstones").toArray()) {
            const auto o = v.toObject();
            SyncTombstone t;
            t.kind = o.value("kind").toString();
            t.id = o.value("id").toString();
            t.mtime = static_cast<qint64>(o.value("mtime").toDouble());
            if (!t.id.isEmpty()) tombstones.append(t);
        }
    }

    void SyncStore::saveState() const {
        QJsonObject o;
        o.insert("server", server);
        o.insert("username", username);
        o.insert("token", token);
        o.insert("device_id", deviceId);
        o.insert("device_name", deviceName);
        o.insert("auto_sync", autoSync);
        o.insert("use_proxy", useProxy);
        o.insert("pending", pending);
        o.insert("last_version", static_cast<double>(lastVersion));
        o.insert("last_sync_at", static_cast<double>(lastSyncAt));
        o.insert("last_sync_device", lastSyncDevice);
        o.insert("last_error", lastError);
        WriteObject(kStateFile, o);
    }

    void SyncStore::saveMeta() const {
        QJsonObject itemsObj;
        for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
            QJsonObject e;
            e.insert("hash", it.value().hash);
            e.insert("mtime", static_cast<double>(it.value().mtime));
            itemsObj.insert(it.key(), e);
        }
        QJsonArray tombs;
        for (const auto &t: tombstones) {
            QJsonObject e;
            e.insert("kind", t.kind);
            e.insert("id", t.id);
            e.insert("mtime", static_cast<double>(t.mtime));
            tombs.append(e);
        }
        QJsonObject o;
        o.insert("items", itemsObj);
        o.insert("tombstones", tombs);
        WriteObject(kMetaFile, o);
    }

    void SyncStore::clearSession() {
        token.clear();
        items.clear();
        tombstones.clear();
        lastVersion = 0;
        lastSyncAt = 0;
        lastSyncDevice.clear();
        lastError.clear();
        pending = false;
        saveState();
        saveMeta();
    }

} // namespace NekoGui_sync
