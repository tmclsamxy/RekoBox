package io.rekobox.android.sync

import io.rekobox.android.database.DataStore
import io.rekobox.android.database.GroupManager
import io.rekobox.android.database.ProfileManager
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
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

/**
 * 订阅引擎：登录后从服务端**只读**拉取节点池，落到本地分组。
 *
 * 与 Windows 端共用同一份协议与服务器：
 *   GET /api/v1/nodes[?version=N] → {version, count, read_only, nodes:[{id,link,name,group,...}]}
 *
 * 服务端由管理员维护节点池，客户端没有任何上传接口，所以本地增删改只影响本机。
 * 一次拉取：服务端新增 → 本地新建；链接变了 → 覆盖本地那一条；服务端删除/禁用 → 本地删除
 * （正在使用的节点跳过，等用户切走后再删）。
 */
object SyncEngine {

    enum class State { LOGGED_OUT, IDLE, PULLING, OFFLINE, ERROR }

    @Volatile
    var state: State = State.LOGGED_OUT
        private set

    @Volatile
    var lastMessage: String = ""
        private set

    /** UI 刷新回调（主线程由调用方切） */
    @Volatile
    var onChange: (() -> Unit)? = null

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val mutex = Mutex()
    private var retryJob: Job? = null
    private var timerJob: Job? = null
    private var retryDelayMs = 5_000L
    private var inited = false

    private const val DEFAULT_GROUP = "云端节点"
    private const val MIN_PULL_INTERVAL_MS = 60_000L

    /** 一次拉取的结果 */
    data class Stats(
        val created: Int = 0,
        val updated: Int = 0,
        val removed: Int = 0,
        val skipped: Int = 0,
    ) {
        val changed: Boolean get() = created + updated + removed > 0
    }

    fun loggedIn(): Boolean = SyncStore.loggedIn()

    fun statusText(): String = when {
        !loggedIn() -> "未登录（仅本地使用）"
        state == State.PULLING -> "正在拉取节点…"
        state == State.OFFLINE ->
            "离线，稍后重试${if (SyncStore.lastError.isNotEmpty()) "：${SyncStore.lastError}" else ""}"

        state == State.ERROR -> "拉取失败：${SyncStore.lastError}"
        SyncStore.lastSyncAt > 0 ->
            "上次拉取：${formatTime(SyncStore.lastSyncAt)} · ${SyncStore.lastCount} 个节点（只读）"

        else -> "已登录，尚未拉取"
    }

    fun init() {
        if (inited) return
        inited = true
        SyncStore.load()
        state = if (loggedIn()) State.IDLE else State.LOGGED_OUT
        restartTimer()

        // 启动 3 秒后拉一次：等界面与数据库就绪，避免刷新时机太早
        scope.launch {
            delay(3000)
            if (loggedIn()) syncNow("startup")
        }
    }

    /** 按当前开关与间隔重排定时拉取 */
    fun restartTimer() {
        timerJob?.cancel()
        timerJob = null
        if (!loggedIn() || !SyncStore.autoPull) return
        val interval = SyncStore.pullIntervalMin * 60_000L
        timerJob = scope.launch {
            while (true) {
                delay(interval.coerceAtLeast(MIN_PULL_INTERVAL_MS))
                if (loggedIn() && SyncStore.autoPull) syncNow("timer")
            }
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
            val body = try {
                JSONObject(r.body ?: "")
            } catch (_: Exception) {
                JSONObject()
            }
            val token = body.optString("token")
            if (token.isEmpty()) {
                callback(false, "服务端没有返回令牌")
                return@launch
            }
            if (SyncStore.username != username.trim()) {
                // 换账号：旧的落地记录必须清掉，否则会把 A 的节点当成 B 的
                SyncStore.nodes.clear()
                SyncStore.lastVersion = 0
                SyncStore.lastSyncAt = 0
                SyncStore.lastCount = 0
                SyncStore.saveNodes()
            }
            SyncStore.server = server
            SyncStore.username = username.trim()
            SyncStore.token = token
            SyncStore.role = body.optJSONObject("user")?.optString("role") ?: ""
            SyncStore.lastError = ""
            state = State.IDLE
            restartTimer()
            notifyChanged()
            callback(true, "登录成功")
            syncNow("login")
        }
    }

    fun logout(callback: (() -> Unit)? = null) {
        if (!loggedIn()) {
            SyncStore.clearSession()
            state = State.LOGGED_OUT
            restartTimer()
            notifyChanged()
            callback?.invoke()
            return
        }
        scope.launch {
            // 服务端连不上也要本地注销：令牌留在服务端不影响本机继续用
            SyncApi.post(SyncStore.server, "/api/v1/auth/logout", SyncStore.token)
            SyncStore.clearSession()
            timerJob?.cancel()
            state = State.LOGGED_OUT
            notifyChanged()
            callback?.invoke()
        }
    }

    // ---------- 拉取 ----------

    fun syncNow(reason: String) {
        if (!loggedIn()) {
            state = State.LOGGED_OUT
            notifyChanged()
            return
        }
        scope.launch { doPull(reason) }
    }

    private suspend fun doPull(reason: String) {
        if (!mutex.tryLock()) return // 已有拉取在跑，等下一轮
        try {
            state = State.PULLING
            notifyChanged()
            retryJob?.cancel()

            var path = "/api/v1/nodes"
            if (SyncStore.lastVersion > 0) path += "?version=${SyncStore.lastVersion}"
            Logs.d("sync pull: $reason $path")

            val r = SyncApi.get(SyncStore.server, path, SyncStore.token)
            if (!r.ok) {
                fail(r)
                return
            }
            val obj = try {
                JSONObject(r.body ?: "{}")
            } catch (e: Exception) {
                JSONObject()
            }
            if (obj.optBoolean("unchanged")) {
                finish(Stats(), unchanged = true)
                return
            }
            val version = obj.optLong("version")
            val arr = obj.optJSONArray("nodes") ?: JSONArray()
            val stats = applyNodes(version, arr)
            finish(stats, unchanged = false)
        } catch (e: Exception) {
            Logs.w(e)
            SyncStore.lastError = e.message ?: "拉取异常"
            state = State.ERROR
            notifyChanged()
        } finally {
            mutex.unlock()
            SyncStore.saveNodes()
        }
    }

    private suspend fun applyNodes(version: Long, arr: JSONArray): Stats {
        var created = 0
        var updated = 0
        var removed = 0
        var skipped = 0

        val alive = HashSet<String>()
        val groupIds = HashMap<String, Long>()
        SagerDatabase.groupDao.allGroups().forEach { groupIds[it.name ?: ""] = it.id }

        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val id = o.optString("id").trim()
            val link = o.optString("link").trim()
            if (id.isEmpty() || link.isEmpty()) {
                skipped++
                continue
            }
            alive.add(id)

            val hash = sha1(link)
            val ref = SyncStore.nodes[id]
            val existing = ref?.let { SagerDatabase.proxyDao.getById(it.localId) }
            if (existing != null && ref.hash == hash) continue // 链接没变：本地改名/调参保留

            val bean = LinkParser.parse(link)
            if (bean == null) {
                skipped++
                Logs.w("skip unparsable link: ${o.optString("name")}")
                continue
            }

            // 单条节点失败不能拖垮整轮拉取：记日志后继续
            val result = runCatching {
                val gid = ensureGroup(groupIds, o.optString("group"))
                if (existing != null) {
                    existing.putBean(bean)
                    existing.groupId = gid
                    ProfileManager.updateProfile(existing)
                    SyncStore.nodes[id] = SyncStore.NodeRef(existing.id, hash)
                    updated++
                } else {
                    val profile = ProfileManager.createProfile(gid, bean)
                    SyncStore.nodes[id] = SyncStore.NodeRef(profile.id, hash)
                    created++
                }
            }
            if (result.isFailure) {
                skipped++
                Logs.w("apply node failed: ${o.optString("name")}", result.exceptionOrNull() ?: Exception())
            }
        }

        // 服务端已删除或已禁用（禁用不下发）
        val gone = SyncStore.nodes.keys.filter { it !in alive }
        for (id in gone) {
            val ref = SyncStore.nodes[id] ?: continue
            val profile = SagerDatabase.proxyDao.getById(ref.localId)
            if (profile == null) {
                SyncStore.nodes.remove(id)
                continue
            }
            if (DataStore.selectedProxy == profile.id) {
                // 正在用这条节点：等用户切走后再删，避免连接被打断
                Logs.w("keep selected profile for later removal: ${profile.displayName()}")
                continue
            }
            ProfileManager.deleteProfile(profile.groupId, profile.id)
            SyncStore.nodes.remove(id)
            removed++
        }

        SyncStore.lastVersion = version
        SyncStore.lastCount = arr.length()
        SyncStore.lastSyncAt = System.currentTimeMillis()
        SyncStore.lastError = ""
        return Stats(created, updated, removed, skipped)
    }

    private suspend fun ensureGroup(cache: HashMap<String, Long>, rawName: String): Long {
        val name = rawName.trim().ifEmpty { DEFAULT_GROUP }
        cache[name]?.let { return it }

        val existing = SagerDatabase.groupDao.allGroups().firstOrNull { it.name == name }
        if (existing != null) {
            cache[name] = existing.id
            return existing.id
        }
        val group = GroupManager.createGroup(ProxyGroup(name = name))
        cache[name] = group.id
        return group.id
    }

    private fun finish(stats: Stats, unchanged: Boolean) {
        retryDelayMs = 5_000
        state = State.IDLE
        SyncStore.lastError = ""
        lastMessage = when {
            unchanged -> "已是最新（${SyncStore.lastCount} 个节点）"
            stats.changed -> buildString {
                append("已更新：新增 ${stats.created} · 更新 ${stats.updated} · 删除 ${stats.removed}")
                if (stats.skipped > 0) append("，${stats.skipped} 条无法解析")
            }

            else -> "已是最新（${SyncStore.lastCount} 个节点）"
        }
        notifyChanged()
    }

    private fun fail(r: SyncApi.Result) {
        if (r.status == 401) {
            // 令牌失效（服务端删号 / 改口令 / 禁用）：清会话退回本地模式，不动本地节点
            SyncStore.clearSession()
            timerJob?.cancel()
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

    // ---------- 工具 ----------

    private fun sha1(text: String): String {
        val bytes = MessageDigest.getInstance("SHA-1").digest(text.toByteArray())
        return bytes.joinToString("") { "%02x".format(it) }
    }

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
        SyncStore.saveNodes()
        onChange?.invoke()
    }
}
