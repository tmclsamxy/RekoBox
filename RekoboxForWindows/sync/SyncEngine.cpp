#include "SyncEngine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <QSysInfo>
#include <QUrl>

#include "db/Database.hpp"
#include "db/ProxyEntity.hpp"
#include "fmt/includes.h"
#include "main/NekoGui.hpp"
#include "sync/SyncModel.hpp"

// GroupUpdater.cpp 里的收尾修正（security 取值归一化），直接复用已有实现
namespace NekoGui_sub {
    void RawUpdater_FixEnt(const std::shared_ptr<NekoGui::ProxyEntity> &ent);
}

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

        QString LinkHash(const QString &link) {
            return QString::fromLatin1(
                QCryptographicHash::hash(link.toUtf8(), QCryptographicHash::Sha1).toHex());
        }

        // 分享链接 -> 实体。只处理客户端能导入的协议，其余返回 nullptr。
        // 与服务端 server/nodes.go 的放行清单保持一致。
        std::shared_ptr<NekoGui::ProxyEntity> ProfileFromLink(const QString &link) {
            std::shared_ptr<NekoGui::ProxyEntity> ent;
            const auto ok = [&ent](bool parsed) { return parsed && ent && ent->bean; };

            if (link.startsWith("socks5://") || link.startsWith("socks4://") ||
                link.startsWith("socks4a://") || link.startsWith("socks://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("socks");
                if (!ok(ent->SocksHTTPBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("http://") || link.startsWith("https://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("http");
                if (!ok(ent->SocksHTTPBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("ss://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("shadowsocks");
                if (!ok(ent->ShadowSocksBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("vmess://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("vmess");
                if (!ok(ent->VMessBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("vless://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("vless");
                if (!ok(ent->TrojanVLESSBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("trojan://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("trojan");
                if (!ok(ent->TrojanVLESSBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("naive+")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("naive");
                if (!ok(ent->NaiveBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("hysteria2://") || link.startsWith("hy2://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("hysteria2");
                if (!ok(ent->QUICBean()->TryParseLink(link))) return nullptr;
            } else if (link.startsWith("tuic://")) {
                ent = NekoGui::ProfileManager::NewProxyEntity("tuic");
                if (!ok(ent->QUICBean()->TryParseLink(link))) return nullptr;
            } else {
                return nullptr;
            }

            NekoGui_sub::RawUpdater_FixEnt(ent);
            return ent;
        }

        QString DefaultGroupName() { return QObject::tr("云端节点"); }
    } // namespace

    SyncEngine *SyncEngine::instance() {
        static SyncEngine *inst = new SyncEngine;
        return inst;
    }

    SyncEngine::SyncEngine(QObject *parent) : QObject(parent) {
        api = new SyncApi(this);

        retryTimer.setSingleShot(true);
        connect(&retryTimer, &QTimer::timeout, this, [this] {
            if (state.loggedIn() && !pulling_) syncNow("retry");
        });

        pullTimer.setSingleShot(false);
        connect(&pullTimer, &QTimer::timeout, this, [this] {
            if (state.loggedIn() && !pulling_) syncNow("timer");
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
        restartTimer();

        // 启动 3 秒后拉一次：等主窗口把节点列表铺好，避免刷新时机太早
        QTimer::singleShot(3000, this, [this] {
            if (state.loggedIn()) syncNow("startup");
        });
    }

    void SyncEngine::persistState() {
        state.saveState();
        state.saveNodes();
    }

    void SyncEngine::restartTimer() {
        pullTimer.stop();
        if (!state.loggedIn() || !state.autoPull) return;
        auto minutes = state.pullIntervalMin;
        if (minutes < 1) minutes = 30;
        pullTimer.start(minutes * 60 * 1000);
    }

    QString SyncEngine::statusText() const {
        switch (current) {
            case State::LoggedOut:
                return QObject::tr("未登录（仅本地使用）");
            case State::Pulling:
                return QObject::tr("正在拉取节点…");
            case State::Offline:
                return QObject::tr("离线，稍后自动重试：%1").arg(state.lastError);
            case State::Error:
                return QObject::tr("拉取失败：%1").arg(state.lastError);
            case State::Idle:
                break;
        }
        QString s;
        if (state.lastSyncAt <= 0) {
            s = QObject::tr("已登录，尚未拉取");
        } else {
            s = QObject::tr("上次拉取：%1 · %2 个节点")
                    .arg(QDateTime::fromMSecsSinceEpoch(state.lastSyncAt).toString("MM-dd HH:mm:ss"))
                    .arg(state.lastCount);
        }
        s += QObject::tr("（只读，客户端不能上传）");
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
        if (pulling_) {
            emit loginFinished(false, QObject::tr("正在拉取节点，请稍候再登录"));
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
                // 换了账号：旧的落地记录不能留，否则会把 A 的节点当成 B 的
                state.nodes.clear();
                state.lastVersion = 0;
                state.lastSyncAt = 0;
                state.lastCount = 0;
            }
            state.username = user;
            state.token = token;
            state.role = r.body.value("user").toObject().value("role").toString();
            state.lastError.clear();
            state.saveState();
            state.saveNodes();
            restartTimer();
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
            pullTimer.stop();
            setState(State::LoggedOut);
            emit syncFinished(true, QObject::tr("已注销，本地已有节点保留"));
        });
    }

    void SyncEngine::noteLocalChange() {
        // 只读模式：本地改动不需要上传。这里刻意留空，
        // 调用点（实体/分组 Save 回调）保持不动，将来要加"仅上传备注"再说。
    }

    // ---- 拉取主流程 ----

    void SyncEngine::syncNow(const QString &reason, bool force) {
        if (!state.loggedIn()) {
            setState(State::LoggedOut);
            return;
        }
        if (pulling_) {
            // 正在拉的时候用户又点了：记下来，这一轮结束后再补一次。
            // 强制请求优先——否则用户点了"立即拉取"却只等到一个 unchanged。
            queued_ = true;
            if (force) queuedForce_ = true;
            return;
        }
        pulling_ = true;
        retryTimer.stop();
        setState(State::Pulling);
        qDebug() << "[sync] pull start:" << reason << (force ? "(force)" : "");
        runPull(0, force);
    }

    void SyncEngine::runPull(int attempt, bool force) {
        prepareApi();
        auto path = QStringLiteral("/api/v1/nodes");
        // force：不带 version，服务端必定走全量分支
        if (!force && state.lastVersion > 0) path += QStringLiteral("?version=%1").arg(state.lastVersion);

        api->get(path, [this, attempt, force](const ApiResult &r) {
            if (!r.ok()) {
                finishFail(r);
                return;
            }
            if (r.body.value("unchanged").toBool()) {
                // 服务端说版本没变。这只能说明它那边没改动，不能说明本机还是完好的：
                // 用户可能删过节点、配置目录也可能被清过。发现本地缺东西就立刻强制拉一次，
                // 否则本地会永远停在这个缺口上。
                const bool throttled = NowMs() - lastRepairAt < 30000;
                if (HasMissingLocalNodes() && !throttled) {
                    lastRepairAt = NowMs();
                    qDebug() << "[sync] unchanged 但本地缺节点，强制全量重拉";
                    // 直接重跑本轮：pulling_ 仍为 true，不会与用户点击并发，
                    // 也不会走 syncNow 的排队逻辑产生多余的第三次请求。
                    runPull(0, true);
                    return;
                }
                finishOk(PullStats{}, true);
                return;
            }
            const auto version = static_cast<qint64>(r.body.value("version").toDouble());
            const auto nodes = r.body.value("nodes").toArray();
            const auto stats = applyNodes(version, nodes);
            finishOk(stats, false);
        });
    }

    // 本地落地记录里有 sync_id，但对应的实体已经不在了（用户手动删了、配置目录被清过）。
    // 这是唯一需要绕过服务端 unchanged 判断的信号。
    bool SyncEngine::HasMissingLocalNodes() const {
        for (auto it = state.nodes.constBegin(); it != state.nodes.constEnd(); ++it) {
            const auto ent = FindProfileBySync(it.value().syncId);
            if (ent == nullptr || !ent->bean) return true;
        }
        return false;
    }

    PullStats SyncEngine::applyNodes(qint64 version, const QJsonArray &nodes) {
        applying_ = true;
        PullStats st;
        auto *pm = NekoGui::profileManager;
        QSet<QString> alive;
        QHash<int, QList<int>> orderByGroup;

        for (const auto &v: nodes) {
            const auto o = v.toObject();
            const auto id = o.value("id").toString().trimmed();
            const auto link = o.value("link").toString().trimmed();
            if (id.isEmpty() || link.isEmpty()) {
                st.skipped++;
                continue;
            }
            alive.insert(id);

            const auto syncId = QStringLiteral("srv:") + id;
            const auto hash = LinkHash(link);
            const auto gid = ensureGroup(o.value("group").toString());

            auto ent = FindProfileBySync(syncId);
            const auto ref = state.nodes.value(id);
            const bool known = !ref.syncId.isEmpty();

            if (ent && known && ref.linkHash == hash) {
                // 链接没变：保持本地现状（用户的改名、调参不会被覆盖）
                orderByGroup[gid].append(ent->id);
                continue;
            }

            auto fresh = ProfileFromLink(link);
            if (!fresh) {
                st.skipped++;
                // 解析不了且本地没有对应实体：这条记录永远无法落地，
                // 留着它会让 HasMissingLocalNodes() 恒为真，触发无休止的强制重拉。
                // 直接丢掉引用；管理员修好链接后服务端版本会变，届时自然会重新建。
                if (ent == nullptr) state.nodes.remove(id);
                qDebug() << "[sync] 跳过无法解析的节点:" << o.value("name").toString() << link.left(48);
                continue;
            }

            if (ent == nullptr) {
                fresh->sync_id = syncId;
                if (!pm->AddProfile(fresh, gid)) {
                    st.skipped++;
                    continue;
                }
                st.created++;
                orderByGroup[gid].append(fresh->id);
            } else if (ent->type != fresh->type) {
                // 管理员把协议换了（比如 tuic → vless）：旧的 bean 类型不兼容，重建一条
                const int oldId = ent->id;
                if (NekoGui::dataStore->started_id == oldId) {
                    st.skipped++; // 正在跑：这一轮先不动，停止后再换
                    orderByGroup[gid].append(oldId);
                    continue;
                }
                pm->DeleteProfile(oldId);
                fresh->sync_id = syncId;
                if (!pm->AddProfile(fresh, gid)) {
                    st.skipped++;
                    continue;
                }
                st.updated++;
                orderByGroup[gid].append(fresh->id);
            } else {
                // 同协议只换 bean 内容：id / gid / 本地统计必须保留，
                // 否则会把文件里的 id 写成 -1，下次启动整条记录就废了
                const int keepId = ent->id;
                ent->FromJson(fresh->ToJson({}));
                ent->id = keepId;
                ent->sync_id = syncId;
                if (ent->gid != gid) {
                    ent->gid = gid;
                }
                ent->Save();
                st.updated++;
                orderByGroup[gid].append(keepId);
            }
            ServerNodeRef nr;
            nr.syncId = syncId;
            nr.linkHash = hash;
            nr.updatedAt = NowMs();
            state.nodes.insert(id, nr);
        }

        // 服务端已删除或已禁用的节点（禁用不会下发）
        QList<QString> gone;
        for (auto it = state.nodes.constBegin(); it != state.nodes.constEnd(); ++it) {
            if (!alive.contains(it.key())) gone.append(it.key());
        }
        for (const auto &id: gone) {
            const auto ref = state.nodes.value(id);
            auto ent = FindProfileBySync(ref.syncId);
            if (ent == nullptr) {
                state.nodes.remove(id);
                continue;
            }
            if (NekoGui::dataStore->started_id == ent->id) {
                // 正在跑这条节点：等用户停止代理后再删，避免连接被打断
                if (MW_show_log) {
                    MW_show_log(QObject::tr("[订阅]「%1」已在服务端移除，停止代理后自动删除")
                                    .arg(ent->bean->DisplayTypeAndName()));
                }
                continue;
            }
            pm->DeleteProfile(ent->id);
            state.nodes.remove(id);
            st.removed++;
        }

        // 组内顺序按服务端顺序落一次
        for (auto it = orderByGroup.constBegin(); it != orderByGroup.constEnd(); ++it) {
            auto g = pm->GetGroup(it.key());
            if (g && g->order != it.value()) {
                g->order = it.value();
                g->Save();
            }
        }

        applying_ = false;

        if (st.changed()) {
            pm->SaveManager();
            if (MW_dialog_message) MW_dialog_message("SyncEngine", "refresh");
        }

        state.lastVersion = version;
        state.lastCount = nodes.size();
        state.lastSyncAt = NowMs();
        state.lastError.clear();
        state.saveState();
        state.saveNodes();

        qDebug() << "[sync] applied: created" << st.created << "updated" << st.updated
                 << "removed" << st.removed << "skipped" << st.skipped;
        return st;
    }

    int SyncEngine::ensureGroup(const QString &name) {
        auto label = name.trimmed();
        if (label.isEmpty()) label = DefaultGroupName();
        const auto syncId = QStringLiteral("srvgrp:") + label;

        if (auto existing = FindGroupBySync(syncId)) return existing->id;

        auto group = NekoGui::ProfileManager::NewGroup();
        group->sync_id = syncId;
        group->name = label;
        if (!NekoGui::profileManager->AddGroup(group)) {
            return NekoGui::dataStore->current_group;
        }
        return group->id;
    }

    void SyncEngine::finishOk(const PullStats &stats, bool unchanged) {
        pulling_ = false;
        state.lastError.clear();
        if (unchanged) {
            // 服务端确认版本没变，等于本次拉取有效：刷新确认时间，
            // 否则用户手动点"立即拉取"后界面的"上次拉取"纹丝不动，看起来像没生效。
            state.lastSyncAt = NowMs();
        }
        state.saveState();
        retryDelayMs = 5000;
        retryTimer.stop();
        setState(State::Idle);

        QString msg;
        if (unchanged) {
            msg = QObject::tr("已是最新（%1 个节点）").arg(state.lastCount);
        } else if (stats.changed()) {
            msg = QObject::tr("已更新：新增 %1 · 更新 %2 · 删除 %3")
                      .arg(stats.created).arg(stats.updated).arg(stats.removed);
            if (stats.skipped > 0) msg += QObject::tr("，%1 条无法解析").arg(stats.skipped);
        } else {
            msg = QObject::tr("已是最新（%1 个节点）").arg(state.lastCount);
        }
        emit syncFinished(true, msg);
        qDebug() << "[sync] done:" << msg;

        if (queued_) {
            queued_ = false;
            const bool force = queuedForce_;
            queuedForce_ = false;
            QTimer::singleShot(300, this, [this, force] { syncNow("queued", force); });
        }
    }

    void SyncEngine::finishFail(const ApiResult &r) {
        pulling_ = false;
        queued_ = false;
        queuedForce_ = false;

        if (r.status == 401) {
            // 令牌失效（服务端删号 / 改口令 / 禁用）：清会话，退回本地模式，不动本地节点
            state.clearSession();
            pullTimer.stop();
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

    void NotifyLocalChange() {
        SyncEngine::instance()->noteLocalChange();
    }

} // namespace NekoGui_sync
