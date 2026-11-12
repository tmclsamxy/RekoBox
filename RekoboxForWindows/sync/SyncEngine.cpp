#include "SyncEngine.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <QSysInfo>
#include <QUrl>

#include "db/Database.hpp"
#include "db/ProxyEntity.hpp"
#include "main/NekoGui.hpp"

namespace NekoGui_sync {

    namespace {
        std::shared_ptr<NekoGui::Group> FindGroupBySync(const QString &id) {
            if (id.isEmpty()) return nullptr;
            for (const auto &[gid, group]: NekoGui::profileManager->groups) {
                if (group && group->sync_id == id) return group;
            }
            return nullptr;
        }

        std::shared_ptr<NekoGui::ProxyEntity> FindProfileBySync(const QString &id) {
            if (id.isEmpty()) return nullptr;
            for (const auto &[pid, ent]: NekoGui::profileManager->profiles) {
                if (ent && ent->sync_id == id) return ent;
            }
            return nullptr;
        }

        // 规范化服务器地址：补协议、去尾部斜杠、去掉误填的 /api/v1
        QString NormalizeServer(const QString &in) {
            auto s = in.trimmed();
            if (s.isEmpty()) return {};
            if (!s.startsWith("http://", Qt::CaseInsensitive) && !s.startsWith("https://", Qt::CaseInsensitive)) {
                s.prepend("http://");
            }
            while (s.endsWith('/')) s.chop(1);
            if (s.endsWith("/api/v1", Qt::CaseInsensitive)) s.chop(7);
            QUrl url(s);
            if (!url.isValid() || url.host().isEmpty()) return {};
            return s;
        }

        constexpr qint64 kTombstoneTtlMs = 30LL * 24 * 3600 * 1000; // 墓碑保留 30 天
    } // namespace

    SyncEngine *SyncEngine::instance() {
        static SyncEngine *inst = new SyncEngine;
        return inst;
    }

    SyncEngine::SyncEngine(QObject *parent) : QObject(parent) {
        api = new SyncApi(this);

        debounce.setSingleShot(true);
        debounce.setInterval(5000); // 连续编辑合并成一次推送
        connect(&debounce, &QTimer::timeout, this, [this] {
            if (state.autoSync && state.loggedIn() && !syncing_) syncNow("local-change");
        });

        retryTimer.setSingleShot(true);
        connect(&retryTimer, &QTimer::timeout, this, [this] {
            if (state.loggedIn() && !syncing_) syncNow("retry");
        });
    }

    void SyncEngine::init() {
        if (inited) return;
        inited = true;

        state.load();
        if (state.deviceName.isEmpty()) state.deviceName = QSysInfo::machineHostName();
        if (state.deviceName.isEmpty()) state.deviceName = QStringLiteral("windows-pc");
        if (state.deviceId.isEmpty()) state.deviceId = NewSyncId();
        state.saveState();

        setState(state.loggedIn() ? State::Idle : State::LoggedOut);

        // 启动 3 秒后拉一次：等主窗口把节点列表铺好，避免刷新时机太早
        QTimer::singleShot(3000, this, [this] {
            if (state.loggedIn()) syncNow("startup");
        });
    }

    void SyncEngine::persistState() {
        state.saveState();
        state.saveMeta();
    }

    QString SyncEngine::statusText() const {
        switch (current) {
            case State::LoggedOut:
                return QObject::tr("未登录（仅本地使用）");
            case State::Syncing:
                return QObject::tr("正在同步…");
            case State::Offline:
                return QObject::tr("离线，稍后自动重试：%1").arg(state.lastError);
            case State::Error:
                return QObject::tr("同步失败：%1").arg(state.lastError);
            case State::Idle:
                break;
        }
        QString s;
        if (state.lastSyncAt <= 0) {
            s = QObject::tr("已登录，尚未同步");
        } else {
            s = QObject::tr("上次同步：%1")
                    .arg(QDateTime::fromMSecsSinceEpoch(state.lastSyncAt).toString("MM-dd HH:mm:ss"));
        }
        if (state.pending) s += QObject::tr("（有本地改动待上传）");
        return s;
    }

    void SyncEngine::setState(State s, const QString &error) {
        current = s;
        if (!error.isEmpty()) state.lastError = error;
        emit stateChanged();
    }

    void SyncEngine::prepareApi() {
        api->setBaseUrl(state.server);
        api->setToken(state.token);
        const bool useProxy = state.useProxy && NekoGui::dataStore != nullptr;
        api->setProxy(useProxy, useProxy ? NekoGui::dataStore->inbound_socks_port : 0,
                      useProxy ? NekoGui::dataStore->inbound_auth->username : QString(),
                      useProxy ? NekoGui::dataStore->inbound_auth->password : QString());
    }

    // ---- 登录 / 注销 ----

    void SyncEngine::login(const QString &server, const QString &username, const QString &password) {
        const auto url = NormalizeServer(server);
        if (url.isEmpty()) {
            emit loginFinished(false, QObject::tr("服务器地址无效，示例：http://1.2.3.4:8080"));
            return;
        }
        if (username.trimmed().isEmpty() || password.isEmpty()) {
            emit loginFinished(false, QObject::tr("请填写用户名与口令"));
            return;
        }
        if (syncing_) {
            emit loginFinished(false, QObject::tr("正在同步，请稍候再登录"));
            return;
        }

        state.server = url;
        prepareApi();
        api->setToken("");

        QJsonObject body;
        body.insert("username", username.trimmed());
        body.insert("password", password);
        body.insert("device", state.deviceName);

        api->post("/api/v1/auth/login", body, [this, user = username.trimmed()](const ApiResult &r) {
            if (!r.ok()) {
                emit loginFinished(false, r.error.isEmpty() ? QObject::tr("登录失败") : r.error);
                return;
            }
            const auto token = r.body.value("token").toString();
            if (token.isEmpty()) {
                emit loginFinished(false, QObject::tr("服务端没有返回令牌"));
                return;
            }
            if (state.username != user) {
                // 换了账号：旧账号的基线必须清掉，否则会把 A 的删除动作带到 B
                state.items.clear();
                state.tombstones.clear();
                state.lastVersion = 0;
                state.lastSyncAt = 0;
            }
            state.username = user;
            state.token = token;
            state.lastError.clear();
            state.pending = false;
            state.saveState();
            emit loginFinished(true, QObject::tr("登录成功"));
            emit stateChanged();
            syncNow("login");
        });
    }

    void SyncEngine::logout() {
        if (!state.loggedIn()) {
            state.clearSession();
            setState(State::LoggedOut);
            return;
        }
        prepareApi();
        // 服务端连不上也要本地注销：令牌留在服务端不影响本机继续用
        api->post("/api/v1/auth/logout", {}, [this](const ApiResult &) {
            state.clearSession();
            setState(State::LoggedOut);
            emit syncFinished(true, QObject::tr("已注销，本地数据保留"));
        });
    }

    void SyncEngine::noteLocalChange() {
        if (applying_) return; // 应用远端数据引发的保存不算本地改动
        if (!state.loggedIn()) return;
        if (!state.pending) {
            state.pending = true;
            state.saveState();
            emit stateChanged();
        }
        if (state.autoSync) debounce.start();
    }

    // ---- 同步主流程 ----

    void SyncEngine::syncNow(const QString &reason) {
        if (!state.loggedIn()) {
            setState(State::LoggedOut);
            return;
        }
        if (syncing_) {
            queued_ = true;
            return;
        }
        syncing_ = true;
        retryTimer.stop();
        setState(State::Syncing);
        qDebug() << "[sync] start:" << reason;
        runRound(0);
    }

    void SyncEngine::runRound(int attempt) {
        prepareApi();
        // 先扫本地：更新指纹、生成墓碑、算出每个条目的 mtime
        const auto local = buildLocalVault();

        api->get("/api/v1/vault", [this, attempt, local](const ApiResult &r) {
            if (!r.ok()) {
                finishFail(r);
                return;
            }

            Vault remote;
            qint64 baseVersion = 0;
            if (r.status == 200) {
                baseVersion = static_cast<qint64>(r.body.value("version").toDouble());
                remote = VaultFromJson(r.body.value("payload").toObject());
            }

            int conflicts = 0;
            const auto merged = MergeVault(local, remote, &conflicts);
            applyVault(merged);

            // 本地已经和远端一致（比如只是打开看了一眼）：不必制造新版本
            if (VaultEquals(merged, remote)) {
                finishOk(conflicts, false);
                return;
            }

            QJsonObject putBody;
            putBody.insert("base_version", static_cast<double>(baseVersion));
            putBody.insert("device", state.deviceName);
            putBody.insert("payload", VaultToJson(merged));

            api->put("/api/v1/vault", putBody, [this, attempt, conflicts](const ApiResult &pr) {
                if (pr.ok()) {
                    state.lastVersion = static_cast<qint64>(pr.body.value("version").toDouble());
                    finishOk(conflicts, true);
                    return;
                }
                if (pr.status == 409 && attempt < 3) {
                    // 另一台设备先写了：重新拉取、重新合并，最多 3 轮
                    qDebug() << "[sync] version conflict, retry round" << attempt + 1;
                    runRound(attempt + 1);
                    return;
                }
                finishFail(pr);
            });
        });
    }

    void SyncEngine::finishOk(int conflicts, bool pushed) {
        syncing_ = false;
        state.lastSyncAt = NowMs();
        state.pending = false;
        state.lastError.clear();
        state.saveState();
        retryDelayMs = 5000;
        retryTimer.stop();
        setState(State::Idle);

        QString msg = pushed ? QObject::tr("已上传本地改动") : QObject::tr("已是最新");
        if (conflicts > 0) msg += QObject::tr("，合并 %1 处冲突").arg(conflicts);
        emit syncFinished(true, msg);
        qDebug() << "[sync] done:" << msg;

        if (queued_) {
            queued_ = false;
            QTimer::singleShot(300, this, [this] { syncNow("queued"); });
        }
    }

    void SyncEngine::finishFail(const ApiResult &r) {
        syncing_ = false;
        queued_ = false;

        if (r.status == 401) {
            // 令牌失效（服务端删号 / 改口令）：清会话，退回本地模式，不动本地节点
            state.clearSession();
            setState(State::LoggedOut, r.error.isEmpty() ? QObject::tr("登录已失效，请重新登录") : r.error);
            emit syncFinished(false, state.lastError);
            return;
        }

        state.lastError = r.error.isEmpty() ? QObject::tr("未知错误") : r.error;
        state.saveState();
        if (r.networkError()) {
            setState(State::Offline, state.lastError);
            scheduleRetry();
        } else {
            setState(State::Error, state.lastError);
            if (r.status >= 500) scheduleRetry();
        }
        emit syncFinished(false, state.lastError);
        qDebug() << "[sync] failed:" << r.status << state.lastError;
    }

    void SyncEngine::scheduleRetry() {
        if (!state.loggedIn()) return;
        retryTimer.setInterval(retryDelayMs);
        retryTimer.start();
        retryDelayMs = qMin(retryDelayMs * 2, 300000); // 封顶 5 分钟
    }

    // ---- 本地 -> 报文 ----

    void SyncEngine::stampItem(SyncItem &item, bool firstBaseline) {
        const auto key = SyncStore::key(item.kind, item.id);
        const auto hash = ItemHash(item.data);
        auto it = state.items.find(key);
        const qint64 now = NowMs();

        if (it == state.items.end()) {
            // 首次见到的条目。首次建立基线时把 mtime 压到最小，
            // 让「服务端已有数据」在冲突中优先，避免新登录的机器覆盖云端
            item.mtime = firstBaseline ? 1 : now;
            state.items.insert(key, SyncMetaEntry{hash, item.mtime});
            return;
        }
        if (it->hash.isEmpty()) {
            // 内容刚从远端应用下来：采信当前序列化结果，保留远端 mtime
            it->hash = hash;
            item.mtime = it->mtime;
            return;
        }
        if (it->hash != hash) {
            item.mtime = now;
            it->hash = hash;
            it->mtime = now;
            return;
        }
        item.mtime = it->mtime;
    }

    Vault SyncEngine::buildLocalVault() {
        Vault vault;
        auto *pm = NekoGui::profileManager;
        const bool firstBaseline = state.items.isEmpty() && state.tombstones.isEmpty();

        // 全新设备判定：首次建基线、没有任何节点、只有一个空的默认分组。
        // 这种分组只是安装时自动建的占位，不该被当成"本地数据"推到云端——
        // 否则每台新机器都会在云端多一个「默认」分组。
        if (firstBaseline && pm->profiles.empty() && pm->groups.size() == 1) {
            const auto &only = pm->groups.begin()->second;
            if (only && only->url.trimmed().isEmpty() && only->Profiles().isEmpty()) {
                adoptRemoteGroupId = only->id;
            }
        }

        // 1) 老数据没有 sync_id：现场生成并落盘，之后一直跟着这条记录
        for (auto &[gid, group]: pm->groups) {
            if (group && group->sync_id.isEmpty()) {
                group->sync_id = NewSyncId();
                group->Save();
            }
        }
        for (auto &[pid, ent]: pm->profiles) {
            if (ent && ent->sync_id.isEmpty()) {
                ent->sync_id = NewSyncId();
                ent->Save();
            }
        }

        // 2) 分组（id/order 是机器本地字段，导出时换成 UUID 列表）
        QHash<int, QString> groupSyncOf;
        for (auto &[gid, group]: pm->groups) {
            if (!group) continue;
            groupSyncOf.insert(gid, group->sync_id);

            // 全新设备的占位分组不参与同步
            if (adoptRemoteGroupId == gid) continue;

            SyncItem item;
            item.kind = "group";
            item.id = group->sync_id;
            item.data = group->ToJson({"id", "order", "sync_id"});
            QJsonArray orderIds;
            for (const auto &ent: group->ProfilesWithOrder()) {
                if (ent && !ent->sync_id.isEmpty()) orderIds.append(ent->sync_id);
            }
            item.data.insert("order_ids", orderIds);
            stampItem(item, firstBaseline);
            vault.groups.append(item);
        }

        // 3) 节点（延迟、测试报告、流量统计属于本机状态，不参与同步）
        for (auto &[pid, ent]: pm->profiles) {
            if (!ent) continue;
            SyncItem item;
            item.kind = "profile";
            item.id = ent->sync_id;
            item.groupId = groupSyncOf.value(ent->gid);
            item.data = ent->ToJson({"id", "gid", "yc", "report", "traffic", "sync_id"});
            stampItem(item, firstBaseline);
            vault.profiles.append(item);
        }

        // 4) 上次同步过、这次本地没了的条目 = 用户删掉了 → 生成墓碑
        QSet<QString> seen;
        for (const auto &g: vault.groups) seen.insert(SyncStore::key(g.kind, g.id));
        for (const auto &p: vault.profiles) seen.insert(SyncStore::key(p.kind, p.id));

        QList<QString> vanished;
        for (auto it = state.items.constBegin(); it != state.items.constEnd(); ++it) {
            if (!seen.contains(it.key())) vanished.append(it.key());
        }
        for (const auto &key: vanished) {
            const auto sep = key.indexOf(':');
            if (sep <= 0) continue;
            SyncTombstone t;
            t.kind = key.left(sep);
            t.id = key.mid(sep + 1);
            t.mtime = NowMs();
            state.tombstones.append(t);
            state.items.remove(key);
            qDebug() << "[sync] tombstone" << t.kind << t.id;
        }

        // 5) 过期墓碑清掉，避免无限增长
        const qint64 cutoff = NowMs() - kTombstoneTtlMs;
        for (int i = 0; i < state.tombstones.size();) {
            if (state.tombstones[i].mtime < cutoff) {
                state.tombstones.removeAt(i);
            } else {
                i++;
            }
        }
        for (const auto &t: state.tombstones) {
            SyncItem item;
            item.kind = t.kind;
            item.id = t.id;
            item.mtime = t.mtime;
            item.deleted = true;
            if (t.kind == "group") {
                vault.groups.append(item);
            } else {
                vault.profiles.append(item);
            }
        }

        state.saveMeta();
        return vault;
    }

    // ---- 报文 -> 本地 ----

    void SyncEngine::recordApplied(const SyncItem &item) {
        // hash 留空：下次扫描按实体真实序列化结果补全，避免字段补全导致「假改动」
        state.items.insert(SyncStore::key(item.kind, item.id), SyncMetaEntry{QString(), item.mtime});
    }

    void SyncEngine::applyVault(const Vault &vault) {
        applying_ = true;
        auto *pm = NekoGui::profileManager;
        bool changed = false;

        // 1) 分组先建出来，节点才知道自己该落在哪个分组
        QHash<QString, int> gidOf;
        for (const auto &g: vault.groups) {
            if (g.deleted) continue;
            auto local = FindGroupBySync(g.id);
            if (local == nullptr) {
                local = NekoGui::ProfileManager::NewGroup();
                local->sync_id = g.id;
                local->name = g.data.value("name").toString(QObject::tr("同步分组"));
                if (local->name.isEmpty()) local->name = QObject::tr("同步分组");
                pm->AddGroup(local);
                changed = true;
            }
            local->FromJson(g.data);
            local->sync_id = g.id;
            if (local->name.isEmpty()) local->name = QObject::tr("同步分组");
            local->Save();
            gidOf.insert(g.id, local->id);
            recordApplied(g);
        }

        // 2) 节点：先删除，再新建/覆盖
        for (const auto &p: vault.profiles) {
            auto ent = FindProfileBySync(p.id);
            if (p.deleted) {
                if (ent) {
                    if (NekoGui::dataStore->started_id == ent->id) {
                        // 正在用的节点不能删：把基线的 mtime 对齐墓碑，
                        // 这样合并时「同一时刻」由墓碑优先，停止后下一轮即可删掉
                        state.items.insert(SyncStore::key(p.kind, p.id), SyncMetaEntry{QString(), p.mtime});
                        if (MW_show_log) {
                            MW_show_log(QObject::tr("[同步] 节点正在使用，停止后才会删除：%1").arg(ent->bean->DisplayTypeAndName()));
                        }
                    } else {
                        pm->DeleteProfile(ent->id);
                        state.items.remove(SyncStore::key(p.kind, p.id));
                        changed = true;
                    }
                } else {
                    state.items.remove(SyncStore::key(p.kind, p.id));
                }
                continue;
            }

            int gid = gidOf.value(p.groupId, -1);
            if (gid < 0) gid = NekoGui::dataStore->current_group;

            if (ent == nullptr) {
                ent = NekoGui::ProfileManager::NewProxyEntity(p.data.value("type").toString());
                if (!ent || !ent->bean) {
                    qDebug() << "[sync] skip unknown profile type:" << p.data.value("type").toString();
                    continue;
                }
                ent->FromJson(p.data);
                ent->sync_id = p.id;
                if (!pm->AddProfile(ent, gid)) continue;
                changed = true;
            } else {
                ent->FromJson(p.data);
                ent->sync_id = p.id;
                ent->gid = gid;
                ent->Save();
                changed = true;
            }
            recordApplied(p);
        }

        // 3) 分组内排序：等节点都落好本地 id 之后再翻译 order_ids
        for (const auto &g: vault.groups) {
            if (g.deleted) continue;
            auto local = FindGroupBySync(g.id);
            if (!local) continue;
            QList<int> order;
            for (const auto &v: g.data.value("order_ids").toArray()) {
                auto ent = FindProfileBySync(v.toString());
                if (ent) order.append(ent->id);
            }
            if (local->order != order) {
                local->order = order;
                local->Save();
            }
        }

        // 4) 删除分组（节点已在上面按墓碑删完，这里只剩空壳）
        for (const auto &g: vault.groups) {
            if (!g.deleted) continue;
            auto local = FindGroupBySync(g.id);
            if (local && pm->groups.size() > 1) {
                pm->DeleteGroup(local->id);
                changed = true;
                state.items.remove(SyncStore::key(g.kind, g.id));
            } else if (local) {
                // 只剩最后一个分组，DeleteGroup 会拒绝：保留基线交给下一轮
                state.items.insert(SyncStore::key(g.kind, g.id), SyncMetaEntry{QString(), g.mtime});
            } else {
                state.items.remove(SyncStore::key(g.kind, g.id));
            }
        }

        // 5) 全新设备的占位分组：服务端已有分组就丢掉本地这个空壳
        if (adoptRemoteGroupId >= 0) {
            const int gid = adoptRemoteGroupId;
            adoptRemoteGroupId = -1;
            auto holder = pm->GetGroup(gid);
            bool remoteHasGroup = false;
            for (const auto &g: vault.groups) {
                if (!g.deleted) {
                    remoteHasGroup = true;
                    break;
                }
            }
            if (holder && remoteHasGroup && pm->groups.size() > 1) {
                const auto sid = holder->sync_id;
                pm->DeleteGroup(gid);
                state.items.remove(SyncStore::key("group", sid));
                changed = true;
            }
        }

        // 6) 当前分组被删时的兜底
        if (pm->GetGroup(NekoGui::dataStore->current_group) == nullptr && !pm->groups.empty()) {
            NekoGui::dataStore->current_group = pm->groups.begin()->first;
        }

        // 7) 关键一步：按「刚写入本地后的真实内容」重建指纹。
        //    recordApplied 只记了 mtime 并把 hash 留空（表示「待采信」），
        //    这里再扫一遍把 hash 补上——否则指纹永远是空的，
        //    之后用户的真实改动会被当成「已同步」而永远推不上去。
        //    此时 applying_ 仍为 true，扫描引发的 Save 不会触发新的同步。
        buildLocalVault();

        applying_ = false;

        if (changed) {
            pm->SaveManager();
            if (MW_dialog_message) MW_dialog_message("SyncEngine", "refresh");
        }
        state.saveMeta();
    }

    void NotifyLocalChange() {
        SyncEngine::instance()->noteLocalChange();
    }

} // namespace NekoGui_sync
