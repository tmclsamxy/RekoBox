package io.rekobox.android.sync

import io.rekobox.android.database.GroupManager
import io.rekobox.android.database.ProfileManager
import io.rekobox.android.database.ProxyEntity
import io.rekobox.android.database.ProxyGroup
import io.rekobox.android.database.SagerDatabase
import io.rekobox.android.ktx.Logs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import java.io.File

/**
 * 同步调度器：负责「什么时候同步、失败了怎么办」，合并逻辑在 SyncModel，字段映射在 SyncMapper。
 *
 * 与 Windows 端共用同一份协议和服务器，服务端代码不需要任何改动。
 * 离线策略：失败后退避重试（5s → 10s → … → 5min），本地改动先落库并置 pending。
 */
object SyncEngine {

    enum class State { LOGGED_OUT, IDLE, SYNCING, OFFLINE, ERROR }

    @Volatile
    var state: State = State.LOGGED_OUT
        private set

    @Volatile
    var lastMessage: String = ""
        private set

    /** UI 刷新回调（主线程） */
    @Volatile
    var onChange: (() -> Unit)? = null

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val mutex = Mutex()
    private var retryJob: Job? = null
    private var debounceJob: Job? = null
    private var retryDelayMs = 5_000L
    private var inited = false
    private var applying = false

    private const val TOMBSTONE_TTL_MS = 30L * 24 * 3600 * 1000

    fun loggedIn(): Boolean = SyncStore.loggedIn()

    fun statusText(): String = when {
        !loggedIn() -> "未登录（仅本地使用）"
        state == State.SYNCING -> "正在同步…"
        state == State.OFFLINE -> "离线，稍后重试${if (SyncStore.lastError.isNotEmpty()) "：${SyncStore.lastError}" else ""}"
        state == State.ERROR -> "同步失败：${SyncStore.lastError}"
        SyncStore.lastSyncAt > 0 -> "上次同步：${formatTime(SyncStore.lastSyncAt)}" +
            if (SyncStore.pending) "（有本地改动待上传）" else ""

        else -> "已登录，尚未同步"
    }

    fun init() {
        if (inited) return
        inited = true
        SyncStore.load()
        state = if (loggedIn()) State.IDLE else State.LOGGED_OUT

        // 本地增删改 → 防抖推送
        ProfileManager.addListener(object : ProfileManager.Listener {
            override suspend fun onAdd(profile: ProxyEntity) = noteLocalChange()
            override suspend fun onUpdated(profile: ProxyEntity, noTraffic: Boolean) = noteLocalChange()
            override suspend fun onUpdated(data: List<io.rekobox.android.aidl.TrafficData>) {}
            override suspend fun onRemoved(groupId: Long, profileId: Long) = noteLocalChange()
        })

        // 启动 3 秒后拉一次：等界面起来，避免刷新时机太早
        scope.launch {
            delay(3000)
            if (loggedIn()) syncNow("startup")
        }
    }

    // ---------- 登录 / 注销 ----------

    fun login(serverRaw: String, username: String, password: String, callback: (Boolean, String) -> Unit) {
        val server = normalizeServer(serverRaw)
        if (server.isEmpty()) {
            callback(false, "服务器地址无效，示例：http://1.2.3.4:8080")
            return
        }
        if (username.isBlank() || password.isEmpty()) {
            callback(false, "请填写用户名与口令")
            return
        }
        scope.launch {
            val payload = JSONObject()
                .put("username", username.trim())
                .put("password", password)
                .put("device", SyncStore.deviceName)
            val r = SyncApi.post(server, "/api/v1/auth/login", "", payload)
            if (!r.ok) {
                callback(false, r.error.ifEmpty { "登录失败" })
                return@launch
            }
            val token = try {
                JSONObject(r.body ?: "").optString("token")
            } catch (_: Exception) {
                ""
            }
            if (token.isEmpty()) {
                callback(false, "服务端没有返回令牌")
                return@launch
            }
            if (SyncStore.username != username.trim()) {
                // 换账号：旧基线必须清掉，否则会把 A 的删除动作带到 B
                SyncStore.items.clear()
                SyncStore.tombstones.clear()
                SyncStore.foreign.clear()
                SyncStore.bindings.clear()
                SyncStore.lastVersion = 0
                SyncStore.lastSyncAt = 0
            }
            SyncStore.server = server
            SyncStore.username = username.trim()
            SyncStore.token = token
            SyncStore.lastError = ""
            SyncStore.pending = false
            SyncStore.saveMeta()
            state = State.IDLE
            notifyChanged()
            callback(true, "登录成功")
            syncNow("login")
        }
    }

    fun logout(callback: (() -> Unit)? = null) {
        if (!loggedIn()) {
            SyncStore.clearSession()
            state = State.LOGGED_OUT
            notifyChanged()
            callback?.invoke()
            return
        }
        scope.launch {
            // 服务端连不上也要本地注销
            SyncApi.post(SyncStore.server, "/api/v1/auth/logout", SyncStore.token)
            SyncStore.clearSession()
            state = State.LOGGED_OUT
            notifyChanged()
            callback?.invoke()
        }
    }

    fun noteLocalChange() {
        if (applying) return
        if (!loggedIn()) return
        if (!SyncStore.pending) {
            SyncStore.pending = true
            notifyChanged()
        }
        if (!SyncStore.autoSync) return
        debounceJob?.cancel()
        debounceJob = scope.launch {
            delay(5000)
            syncNow("local-change")
        }
    }

    // ---------- 同步主流程 ----------

    fun syncNow(reason: String) {
        if (!loggedIn()) {
            state = State.LOGGED_OUT
            notifyChanged()
            return
        }
        scope.launch { doSync(reason) }
    }

    private suspend fun doSync(reason: String) {
        if (!mutex.tryLock()) return // 已有同步在跑，等下一轮
        try {
            state = State.SYNCING
            notifyChanged()
            retryJob?.cancel()
            var attempt = 0
            var round = 0
            while (round <= 3) {
                val local = buildLocalVault()
                val remoteResp = SyncApi.get(SyncStore.server, "/api/v1/vault", SyncStore.token)
                if (!remoteResp.ok) {
                    fail(remoteResp)
                    return
                }
                var baseVersion = 0L
                var remote = SyncModel.Vault()
                if (remoteResp.status == 200) {
                    val obj = JSONObject(remoteResp.body ?: "{}")
                    baseVersion = obj.optLong("version")
                    remote = SyncModel.fromJson(obj.optJSONObject("payload") ?: JSONObject())
                }

                val (merged, conflicts) = SyncModel.merge(local, remote)
                applyVault(merged)

                if (SyncModel.equals(merged, remote)) {
                    finish(conflicts, pushed = false)
                    return
                }

                val putBody = JSONObject()
                    .put("base_version", baseVersion)
                    .put("device", SyncStore.deviceName)
                    .put("payload", SyncModel.toJson(merged))
                val putResp = SyncApi.put(SyncStore.server, "/api/v1/vault", SyncStore.token, putBody)
                if (putResp.ok) {
                    SyncStore.lastVersion = try {
                        JSONObject(putResp.body ?: "{}").optLong("version")
                    } catch (_: Exception) {
                        baseVersion + 1
                    }
                    finish(conflicts, pushed = true)
                    return
                }
                if (putResp.status == 409) {
                    round++
                    continue // 别的设备抢先写了：重拉重合并
                }
                fail(putResp)
                return
            }
            fail(SyncApi.Result(409, "合并冲突重试次数过多"))
        } catch (e: Exception) {
            Logs.w(e)
            SyncStore.lastError = e.message ?: "同步异常"
            state = State.ERROR
            notifyChanged()
        } finally {
            mutex.unlock()
            SyncStore.saveMeta()
        }
    }

    private fun finish(conflicts: Int, pushed: Boolean) {
        SyncStore.lastSyncAt = SyncModel.nowMs()
        SyncStore.pending = false
        SyncStore.lastError = ""
        retryDelayMs = 5_000
        state = State.IDLE
        lastMessage = if (pushed) "已上传本地改动" else "已是最新"
        if (conflicts > 0) lastMessage += "，合并 $conflicts 处冲突"
        notifyChanged()
    }

    private fun fail(r: SyncApi.Result) {
        if (r.status == 401) {
            // 令牌失效（服务端删号 / 改口令）：清会话退回本地模式，不动节点
            SyncStore.clearSession()
            state = State.LOGGED_OUT
            SyncStore.lastError = r.error.ifEmpty { "登录已失效，请重新登录" }
            notifyChanged()
            return
        }
        SyncStore.lastError = r.error
        state = if (r.networkError) State.OFFLINE else State.ERROR
        notifyChanged()
        // 网络错误与 5xx 都退避重试
        if (r.networkError || r.status >= 500) {
            retryJob?.cancel()
            retryJob = scope.launch {
                delay(retryDelayMs)
                retryDelayMs = (retryDelayMs * 2).coerceAtMost(300_000)
                if (loggedIn()) syncNow("retry")
            }
        }
    }

    // ---------- 本地 -> 报文 ----------

    private fun stamp(item: SyncModel.Item, firstBaseline: Boolean) {
        val key = SyncStore.key(item.kind, item.id)
        val hash = item.contentHash()
        val now = SyncModel.nowMs()
        val meta = SyncStore.items[key]
        when {
            meta == null -> {
                item.mtime = if (firstBaseline) 1 else now
                SyncStore.items[key] = SyncStore.MetaEntry(hash, item.mtime)
            }

            meta.hash.isEmpty() -> {
                // 内容刚从远端应用下来：采信当前内容，保留远端 mtime
                meta.hash = hash
                item.mtime = meta.mtime
            }

            meta.hash != hash -> {
                item.mtime = now
                meta.hash = hash
                meta.mtime = now
            }

            else -> item.mtime = meta.mtime
        }
    }

    private fun buildLocalVault(): SyncModel.Vault {
        val vault = SyncModel.Vault()
        val firstBaseline = SyncStore.items.isEmpty() && SyncStore.tombstones.isEmpty()
        val groups = SagerDatabase.groupDao.allGroups()
        val profiles = SagerDatabase.proxyDao.getAll()

        // 绑定 uuid（老数据第一次同步时现场生成）
        groups.forEach { g ->
            if (g.id <= 0) return@forEach
            if (SyncStore.uuidOf(SyncModel.KIND_GROUP, g.id) == null) {
                SyncStore.bind(SyncModel.KIND_GROUP, g.id, SyncModel.newSyncId())
            }
        }
        profiles.forEach { p ->
            if (p.id <= 0) return@forEach
            if (SyncStore.uuidOf(SyncModel.KIND_PROFILE, p.id) == null) {
                SyncStore.bind(SyncModel.KIND_PROFILE, p.id, SyncModel.newSyncId())
            }
        }

        // 分组
        groups.forEach { g ->
            if (g.id <= 0) return@forEach
            val uuid = SyncStore.uuidOf(SyncModel.KIND_GROUP, g.id) ?: return@forEach
            val data = SyncMapper.exportGroup(g)
            if (data.length() == 0) return@forEach
            val item = SyncModel.Item(SyncModel.KIND_GROUP, uuid, 0, data = data)
            stamp(item, firstBaseline)
            vault.groups.add(item)
        }

        // 节点：只同步能映射的类型；其余原样跳过（不会删、不会改）
        profiles.forEach { p ->
            if (p.id <= 0) return@forEach
            val uuid = SyncStore.uuidOf(SyncModel.KIND_PROFILE, p.id) ?: return@forEach
            val exported = SyncMapper.exportProfile(p) ?: return@forEach
            val data = JSONObject().put("type", exported.first).put("bean", exported.second)
            val groupUuid = SyncStore.uuidOf(SyncModel.KIND_GROUP, p.groupId) ?: ""
            val item = SyncModel.Item(SyncModel.KIND_PROFILE, uuid, 0, groupId = groupUuid, data = data)
            stamp(item, firstBaseline)
            vault.profiles.add(item)
        }

        // 墓碑：绑定过但本地已删除
        val aliveProfiles = profiles.map { it.id }.toSet()
        val aliveGroups = groups.map { it.id }.toSet()
        val vanished = SyncStore.bindings.filter { (k, _) ->
            when {
                k.startsWith("p:") -> k.substring(2).toLongOrNull()?.let { it !in aliveProfiles } ?: true
                k.startsWith("g:") -> k.substring(2).toLongOrNull()?.let { it !in aliveGroups } ?: true
                else -> false
            }
        }
        vanished.forEach { (k, uuid) ->
            val kind = if (k.startsWith("g:")) SyncModel.KIND_GROUP else SyncModel.KIND_PROFILE
            SyncStore.tombstones.add(
                SyncModel.Item(kind, uuid, SyncModel.nowMs(), deleted = true)
            )
            SyncStore.items.remove(SyncStore.key(kind, uuid))
            SyncStore.bindings.remove(k)
        }

        // 过期墓碑清理
        val cutoff = SyncModel.nowMs() - TOMBSTONE_TTL_MS
        SyncStore.tombstones.removeAll { it.mtime < cutoff }

        vault.groups.addAll(SyncStore.tombstones.filter { it.kind == SyncModel.KIND_GROUP })
        vault.profiles.addAll(SyncStore.tombstones.filter { it.kind == SyncModel.KIND_PROFILE })

        // 本端看不懂的远端条目：原样带回去，避免把别端的节点抹掉
        SyncStore.foreign.forEach { (key, obj) ->
            val kind = if (key.startsWith("group:")) SyncModel.KIND_GROUP else SyncModel.KIND_PROFILE
            val item = SyncModel.Item(
                kind = kind,
                id = obj.optString("id"),
                mtime = obj.optLong("mtime"),
                groupId = obj.optString("group"),
                deleted = obj.optBoolean("deleted"),
                data = obj.optJSONObject("data"),
            )
            if (item.id.isEmpty()) return@forEach
            if (kind == SyncModel.KIND_GROUP) vault.groups.add(item) else vault.profiles.add(item)
        }

        return vault
    }

    // ---------- 报文 -> 本地 ----------

    private suspend fun applyVault(vault: SyncModel.Vault) {
        applying = true
        try {
            val existingGroups = SagerDatabase.groupDao.allGroups()
            val existingProfiles = SagerDatabase.proxyDao.getAll()

            // 1) 分组先建，节点才知道落到哪
            vault.groups.filter { !it.deleted }.forEach { item ->
                val localId = SyncStore.localIdOf(SyncModel.KIND_GROUP, item.id)
                val local = existingGroups.firstOrNull { it.id == localId }
                val data = item.data ?: return@forEach
                if (local == null) {
                    val group = ProxyGroup(name = data.optString("name").ifEmpty { "同步分组" })
                    group.userOrder = SagerDatabase.groupDao.nextOrder() ?: 1
                    val newId = SagerDatabase.groupDao.createGroup(group)
                    SyncStore.bind(SyncModel.KIND_GROUP, newId, item.id)
                } else {
                    SyncMapper.applyGroup(local, data)
                    SagerDatabase.groupDao.updateGroup(local)
                }
                SyncStore.items[SyncStore.key(SyncModel.KIND_GROUP, item.id)] =
                    SyncStore.MetaEntry("", item.mtime)
            }

            // 2) 节点
            vault.profiles.forEach { item ->
                if (item.deleted) {
                    val localId = SyncStore.localIdOf(SyncModel.KIND_PROFILE, item.id)
                    if (localId != null) {
                        ProfileManager.deleteProfile2(0, localId)
                        SyncStore.unbind(SyncModel.KIND_PROFILE, localId)
                    }
                    SyncStore.items.remove(SyncStore.key(SyncModel.KIND_PROFILE, item.id))
                    SyncStore.foreign.remove(SyncStore.key(SyncModel.KIND_PROFILE, item.id))
                    return@forEach
                }

                val data = item.data ?: return@forEach
                val typeName = data.optString("type")

                // 能解析的才动；不能解析的原样存着，下次同步带回去
                val bean = SyncMapper.importBean(data)
                if (bean == null) {
                    SyncStore.foreign[SyncStore.key(SyncModel.KIND_PROFILE, item.id)] = JSONObject()
                        .put("id", item.id)
                        .put("mtime", item.mtime)
                        .put("group", item.groupId)
                        .put("data", data)
                    return@forEach
                }

                val groupId = SyncStore.localIdOf(SyncModel.KIND_GROUP, item.groupId)
                    ?: existingGroups.firstOrNull()?.id
                    ?: return@forEach
                val localId = SyncStore.localIdOf(SyncModel.KIND_PROFILE, item.id)
                if (localId == null) {
                    val profile = ProfileManager.createProfile(groupId, bean)
                    SyncStore.bind(SyncModel.KIND_PROFILE, profile.id, item.id)
                } else {
                    val local = existingProfiles.firstOrNull { it.id == localId }
                    if (local != null) {
                        local.putBean(bean)
                        local.groupId = groupId
                        ProfileManager.updateProfile(local)
                    }
                }
                SyncStore.items[SyncStore.key(SyncModel.KIND_PROFILE, item.id)] =
                    SyncStore.MetaEntry("", item.mtime)
            }

            // 3) 删除分组（节点已按墓碑先删完）
            vault.groups.filter { it.deleted }.forEach { item ->
                val localId = SyncStore.localIdOf(SyncModel.KIND_GROUP, item.id)
                if (localId != null && SagerDatabase.groupDao.allGroups().size > 1) {
                    GroupManager.deleteGroup(localId)
                    SyncStore.unbind(SyncModel.KIND_GROUP, localId)
                }
                SyncStore.items.remove(SyncStore.key(SyncModel.KIND_GROUP, item.id))
                SyncStore.foreign.remove(SyncStore.key(SyncModel.KIND_GROUP, item.id))
            }
        } finally {
            SyncStore.saveMeta()
            applying = false
        }
    }

    // ---------- 工具 ----------

    private fun normalizeServer(input: String): String {
        var s = input.trim()
        if (s.isEmpty()) return ""
        if (!s.startsWith("http://", true) && !s.startsWith("https://", true)) s = "http://$s"
        while (s.endsWith("/")) s = s.dropLast(1)
        if (s.endsWith("/api/v1", true)) s = s.dropLast(7)
        if (!s.startsWith("http")) return ""
        return s
    }

    private fun formatTime(ms: Long): String {
        val fmt = java.text.SimpleDateFormat("MM-dd HH:mm:ss", java.util.Locale.getDefault())
        return fmt.format(java.util.Date(ms))
    }

    private fun notifyChanged() {
        SyncStore.saveMeta()
        onChange?.invoke()
    }
}
