package io.rekobox.android.sync

import android.content.Context
import android.os.Build
import android.provider.Settings
import io.rekobox.android.SagerNet
import org.json.JSONObject
import java.io.File

/**
 * 订阅的本地状态。
 *
 * - SharedPreferences("rekosync")：服务器、账号、令牌（永久有效）、角色、上次版本/时间、节点数、自动拉取
 * - files/sync/nodes.json：服务端节点 id → 本地节点 id 与上次应用的链接指纹
 *
 * 服务端是**只读节点池**，客户端没有上传通道，所以这里不再保存墓碑、待推送标记之类的写状态。
 * 也没有改动 Room 表结构：服务端 id 与本地 id 的对应关系全部放在 nodes.json 里。
 */
object SyncStore {

    private const val PREFS = "rekosync"

    private val prefs by lazy {
        SagerNet.application.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    }

    private val dir by lazy { File(SagerNet.application.filesDir, "sync").apply { mkdirs() } }
    private val nodesFile by lazy { File(dir, "nodes.json") }

    var server: String
        get() = prefs.getString("server", "") ?: ""
        set(v) = prefs.edit().putString("server", v).apply()

    var username: String
        get() = prefs.getString("username", "") ?: ""
        set(v) = prefs.edit().putString("username", v).apply()

    var token: String
        get() = prefs.getString("token", "") ?: ""
        set(v) = prefs.edit().putString("token", v).apply()

    /** admin / user（服务端登录时返回） */
    var role: String
        get() = prefs.getString("role", "") ?: ""
        set(v) = prefs.edit().putString("role", v).apply()

    /** 是否定时自动拉取节点 */
    var autoPull: Boolean
        get() = prefs.getBoolean("auto_pull", true)
        set(v) = prefs.edit().putBoolean("auto_pull", v).apply()

    /** 自动拉取间隔（分钟） */
    var pullIntervalMin: Int
        get() = prefs.getInt("pull_interval", 30).coerceAtLeast(1)
        set(v) = prefs.edit().putInt("pull_interval", v.coerceAtLeast(1)).apply()

    /** 上次拉到的节点池版本，用来让服务端直接回 unchanged */
    var lastVersion: Long
        get() = prefs.getLong("last_version", 0)
        set(v) = prefs.edit().putLong("last_version", v).apply()

    var lastSyncAt: Long
        get() = prefs.getLong("last_sync_at", 0)
        set(v) = prefs.edit().putLong("last_sync_at", v).apply()

    var lastCount: Int
        get() = prefs.getInt("last_count", 0)
        set(v) = prefs.edit().putInt("last_count", v).apply()

    var lastError: String
        get() = prefs.getString("last_error", "") ?: ""
        set(v) = prefs.edit().putString("last_error", v).apply()

    val deviceName: String by lazy {
        val model = Build.MODEL ?: "android"
        val brand = Build.BRAND ?: ""
        if (brand.isNotEmpty() && !model.startsWith(brand, true)) "$brand $model" else model
    }

    val deviceId: String by lazy {
        try {
            Settings.Secure.getString(SagerNet.application.contentResolver, Settings.Secure.ANDROID_ID)
                ?: "android"
        } catch (_: Exception) {
            "android"
        }
    }

    /** 一条云端节点在本机的落地记录 */
    data class NodeRef(val localId: Long, val hash: String)

    /** key = 服务端节点 id */
    val nodes = HashMap<String, NodeRef>()

    fun loggedIn(): Boolean = token.isNotEmpty() && server.isNotEmpty() && username.isNotEmpty()

    fun isAdmin(): Boolean = role == "admin"

    fun load() {
        nodes.clear()
        if (!nodesFile.exists()) return
        try {
            val obj = JSONObject(nodesFile.readText())
            obj.optJSONObject("nodes")?.let { m ->
                m.keys().forEach { k ->
                    val o = m.optJSONObject(k) ?: return@forEach
                    val localId = o.optLong("local_id")
                    if (localId > 0) nodes[k] = NodeRef(localId, o.optString("hash"))
                }
            }
        } catch (_: Exception) {
            // 坏了就当首次拉取：节点会按服务端现状重建，最多多几个重复项，不会丢数据
            nodes.clear()
        }
    }

    fun saveNodes() {
        try {
            dir.mkdirs()
            val m = JSONObject()
            nodes.forEach { (k, v) ->
                m.put(k, JSONObject().put("local_id", v.localId).put("hash", v.hash))
            }
            val obj = JSONObject().put("nodes", m)
            // 临时文件 + rename，避免写到一半被杀进程
            val tmp = File(dir, "nodes.json.tmp")
            tmp.writeText(obj.toString())
            if (!tmp.renameTo(nodesFile)) {
                nodesFile.delete()
                tmp.renameTo(nodesFile)
            }
        } catch (_: Exception) {
        }
    }

    fun clearSession() {
        token = ""
        role = ""
        lastVersion = 0
        lastSyncAt = 0
        lastCount = 0
        lastError = ""
        // 落地记录一并清掉：重新登录时按服务端现状重建
        nodes.clear()
        saveNodes()
    }
}
