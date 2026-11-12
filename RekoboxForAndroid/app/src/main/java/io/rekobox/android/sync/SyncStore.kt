package io.rekobox.android.sync

import android.content.Context
import android.os.Build
import android.provider.Settings
import io.rekobox.android.SagerNet
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * 本地同步状态：
 *  - SharedPreferences("rekosync")：服务器、账号、令牌（永久有效）、上次版本/时间、pending
 *  - files/sync/meta.json：每个条目的内容指纹与 mtime、墓碑、以及"看不懂"的远端条目
 *
 * 没有改动 Room 表结构（不加 column），本地 id 与云端 uuid 的映射全部放在 meta 里，
 * 避免数据库迁移风险。
 */
object SyncStore {

    private const val PREFS = "rekosync"

    private val prefs by lazy {
        SagerNet.application.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    }

    private val dir by lazy { File(SagerNet.application.filesDir, "sync").apply { mkdirs() } }
    private val metaFile by lazy { File(dir, "meta.json") }

    var server: String
        get() = prefs.getString("server", "") ?: ""
        set(v) = prefs.edit().putString("server", v).apply()

    var username: String
        get() = prefs.getString("username", "") ?: ""
        set(v) = prefs.edit().putString("username", v).apply()

    var token: String
        get() = prefs.getString("token", "") ?: ""
        set(v) = prefs.edit().putString("token", v).apply()

    var autoSync: Boolean
        get() = prefs.getBoolean("auto_sync", true)
        set(v) = prefs.edit().putBoolean("auto_sync", v).apply()

    var pending: Boolean
        get() = prefs.getBoolean("pending", false)
        set(v) = prefs.edit().putBoolean("pending", v).apply()

    var lastVersion: Long
        get() = prefs.getLong("last_version", 0)
        set(v) = prefs.edit().putLong("last_version", v).apply()

    var lastSyncAt: Long
        get() = prefs.getLong("last_sync_at", 0)
        set(v) = prefs.edit().putLong("last_sync_at", v).apply()

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

    data class MetaEntry(var hash: String, var mtime: Long)

    /** key = "profile:<uuid>" / "group:<uuid>" */
    val items = HashMap<String, MetaEntry>()

    /** 待同步的删除墓碑 */
    val tombstones = ArrayList<SyncModel.Item>()

    /** 本端无法解析的远端条目（原样保留，回传时不丢数据） */
    val foreign = HashMap<String, JSONObject>()

    /**
     * 本地 id <-> 云端 uuid 的绑定。key = "p:<profileId>" / "g:<groupId>"，value = uuid。
     * 存在 meta.json 里，不改 Room 表结构，避免数据库迁移。
     */
    val bindings = HashMap<String, String>()

    fun key(kind: String, id: String) = "$kind:$id"

    fun bindingKey(kind: String, localId: Long) =
        if (kind == SyncModel.KIND_GROUP) "g:$localId" else "p:$localId"

    fun uuidOf(kind: String, localId: Long): String? = bindings[bindingKey(kind, localId)]

    fun localIdOf(kind: String, uuid: String): Long? {
        val prefix = if (kind == SyncModel.KIND_GROUP) "g:" else "p:"
        bindings.forEach { (k, v) -> if (v == uuid && k.startsWith(prefix)) return k.substring(2).toLongOrNull() }
        return null
    }

    fun bind(kind: String, localId: Long, uuid: String) {
        bindings[bindingKey(kind, localId)] = uuid
    }

    fun unbind(kind: String, localId: Long) {
        bindings.remove(bindingKey(kind, localId))
    }

    fun loggedIn(): Boolean = token.isNotEmpty() && server.isNotEmpty() && username.isNotEmpty()

    fun load() {
        items.clear()
        tombstones.clear()
        foreign.clear()
        bindings.clear()
        if (!metaFile.exists()) return
        try {
            val obj = JSONObject(metaFile.readText())
            obj.optJSONObject("bindings")?.let { b ->
                b.keys().forEach { k -> bindings[k] = b.optString(k) }
            }
            obj.optJSONObject("items")?.let { m ->
                m.keys().forEach { k ->
                    val o = m.optJSONObject(k) ?: return@forEach
                    items[k] = MetaEntry(o.optString("hash"), o.optLong("mtime"))
                }
            }
            obj.optJSONArray("tombstones")?.let { arr ->
                for (i in 0 until arr.length()) {
                    val o = arr.optJSONObject(i) ?: continue
                    val id = o.optString("id")
                    if (id.isEmpty()) continue
                    tombstones.add(
                        SyncModel.Item(
                            kind = o.optString("kind", SyncModel.KIND_PROFILE),
                            id = id,
                            mtime = o.optLong("mtime"),
                            deleted = true,
                        )
                    )
                }
            }
            obj.optJSONObject("foreign")?.let { f ->
                f.keys().forEach { k ->
                    f.optJSONObject(k)?.let { foreign[k] = it }
                }
            }
        } catch (e: Exception) {
            // meta 坏了就当下次全新基线，最多多推一次，不会丢本地数据
            items.clear()
            tombstones.clear()
            foreign.clear()
        }
    }

    fun saveMeta() {
        try {
            dir.mkdirs()
            val obj = JSONObject()
            val m = JSONObject()
            items.forEach { (k, v) ->
                m.put(k, JSONObject().put("hash", v.hash).put("mtime", v.mtime))
            }
            obj.put("items", m)
            obj.put(
                "tombstones", JSONArray().apply {
                    tombstones.forEach {
                        put(JSONObject().put("kind", it.kind).put("id", it.id).put("mtime", it.mtime))
                    }
                }
            )
            val f = JSONObject()
            foreign.forEach { (k, v) -> f.put(k, v) }
            obj.put("foreign", f)
            val b = JSONObject()
            bindings.forEach { (k, v) -> b.put(k, v) }
            obj.put("bindings", b)
            // 临时文件 + rename，避免同步过程中被杀进程留下半截 JSON
            val tmp = File(dir, "meta.json.tmp")
            tmp.writeText(obj.toString())
            if (!tmp.renameTo(metaFile)) {
                metaFile.delete()
                tmp.renameTo(metaFile)
            }
        } catch (_: Exception) {
        }
    }

    fun clearSession() {
        token = ""
        pending = false
        lastError = ""
        // 刻意保留 items / tombstones / bindings / foreign：
        // 同一账号重新登录时 uuid 不变，不会把本地节点当成新节点再传一遍（否则会翻倍）。
        // 换账号时由 SyncEngine.login() 清基线。
    }
}
