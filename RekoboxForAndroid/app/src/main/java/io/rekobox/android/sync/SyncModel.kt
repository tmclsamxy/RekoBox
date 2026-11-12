package io.rekobox.android.sync

import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

/**
 * 同步数据模型：报文结构 + 逐条目合并。
 *
 * 与 Windows 端 (RekoboxForWindows/sync) 共用同一套协议与合并语义，服务端不需要任何改动：
 *   {
 *     "schema": 1, "exported_at": <ms>,
 *     "groups":   [ {"id":"<uuid>","mtime":<ms>,"data":{...}} ],
 *     "profiles": [ {"id":"<uuid>","mtime":<ms>,"group":"<uuid>","data":{...}} ]
 *   }
 * 删除用墓碑表示：同结构 + "deleted": true，不带 data。
 */
object SyncModel {

    const val KIND_GROUP = "group"
    const val KIND_PROFILE = "profile"

    const val SCHEMA = 1

    data class Item(
        val kind: String,
        val id: String,
        var mtime: Long,
        var deleted: Boolean = false,
        var groupId: String = "",
        var data: JSONObject? = null,
    ) {
        fun contentHash(): String = data?.let { sha256(canonical(it)) } ?: ""

        fun sameContent(other: Item): Boolean {
            if (deleted != other.deleted) return false
            if (deleted) return true
            if (groupId != other.groupId) return false
            return contentHash() == other.contentHash()
        }
    }

    class Vault(
        val groups: MutableList<Item> = mutableListOf(),
        val profiles: MutableList<Item> = mutableListOf(),
    ) {
        fun isEmpty() = groups.isEmpty() && profiles.isEmpty()
    }

    fun newSyncId(): String = java.util.UUID.randomUUID().toString()

    fun nowMs(): Long = System.currentTimeMillis()

    fun fromJson(obj: JSONObject): Vault {
        val vault = Vault()
        obj.optJSONArray("groups")?.let { arr ->
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val id = o.optString("id")
                if (id.isEmpty()) continue
                vault.groups.add(
                    Item(
                        kind = KIND_GROUP,
                        id = id,
                        mtime = o.optLong("mtime"),
                        deleted = o.optBoolean("deleted"),
                        data = o.optJSONObject("data"),
                    )
                )
            }
        }
        obj.optJSONArray("profiles")?.let { arr ->
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val id = o.optString("id")
                if (id.isEmpty()) continue
                vault.profiles.add(
                    Item(
                        kind = KIND_PROFILE,
                        id = id,
                        mtime = o.optLong("mtime"),
                        deleted = o.optBoolean("deleted"),
                        groupId = o.optString("group"),
                        data = o.optJSONObject("data"),
                    )
                )
            }
        }
        return vault
    }

    fun toJson(vault: Vault): JSONObject {
        val obj = JSONObject()
        obj.put("schema", SCHEMA)
        obj.put("exported_at", nowMs())
        obj.put("groups", JSONArray().apply { vault.groups.sortedBy { it.id }.forEach { put(itemToJson(it)) } })
        obj.put("profiles", JSONArray().apply { vault.profiles.sortedBy { it.id }.forEach { put(itemToJson(it)) } })
        return obj
    }

    private fun itemToJson(item: Item): JSONObject {
        val o = JSONObject()
        o.put("id", item.id)
        o.put("mtime", item.mtime)
        if (item.deleted) o.put("deleted", true)
        if (item.groupId.isNotEmpty() && !item.deleted) o.put("group", item.groupId)
        if (!item.deleted) item.data?.let { o.put("data", it) }
        return o
    }

    /**
     * 逐条目 last-write-wins：
     *  1. 只有一边有 → 采用
     *  2. 内容相同 → 取较大的 mtime
     *  3. 内容不同 → mtime 大的赢，相等时本地赢
     *  4. 同一时刻「删除 vs 编辑」→ 删除优先（保证收敛）
     */
    fun merge(local: Vault, remote: Vault): Pair<Vault, Int> {
        var conflicts = 0
        val merged = Vault()
        val (groups, c1) = mergeList(local.groups, remote.groups)
        val (profiles, c2) = mergeList(local.profiles, remote.profiles)
        merged.groups.addAll(groups)
        merged.profiles.addAll(profiles)
        conflicts = c1 + c2
        return merged to conflicts
    }

    private fun mergeList(local: List<Item>, remote: List<Item>): Pair<List<Item>, Int> {
        var conflicts = 0
        val map = sortedMapOf<String, Item>()
        local.forEach { if (it.id.isNotEmpty()) map[it.id] = it }
        remote.forEach { r ->
            if (r.id.isEmpty()) return@forEach
            val l = map[r.id]
            if (l == null) {
                map[r.id] = r
                return@forEach
            }
            if (l.sameContent(r)) {
                if (r.mtime > l.mtime) l.mtime = r.mtime
                return@forEach
            }
            if (!l.deleted && !r.deleted) conflicts++
            if (l.mtime == r.mtime && l.deleted != r.deleted) {
                if (r.deleted) map[r.id] = r
                return@forEach
            }
            if (r.mtime > l.mtime) map[r.id] = r
        }
        return map.values.toList() to conflicts
    }

    fun equals(a: Vault, b: Vault): Boolean {
        if (a.groups.size != b.groups.size || a.profiles.size != b.profiles.size) return false
        fun same(x: List<Item>, y: List<Item>): Boolean {
            for (i in x.indices) {
                if (x[i].id != y[i].id) return false
                if (x[i].mtime != y[i].mtime) return false
                if (!x[i].sameContent(y[i])) return false
            }
            return true
        }
        return same(a.groups, b.groups) && same(a.profiles, b.profiles)
    }

    // ---- 工具 ----

    /** 键排序后的紧凑 JSON，保证同一内容每次哈希结果一致 */
    fun canonical(value: Any?): String = when (value) {
        null, JSONObject.NULL -> "null"
        is JSONObject -> value.keys().asSequence().toList().sorted()
            .joinToString(prefix = "{", postfix = "}", separator = ",") { k ->
                JSONObject.quote(k) + ":" + canonical(value.opt(k))
            }

        is JSONArray -> (0 until value.length())
            .joinToString(prefix = "[", postfix = "]", separator = ",") { canonical(value.opt(it)) }

        is String -> JSONObject.quote(value)
        is Number, is Boolean -> value.toString()
        else -> JSONObject.quote(value.toString())
    }

    fun sha256(text: String): String {
        val d = MessageDigest.getInstance("SHA-256").digest(text.toByteArray())
        return d.joinToString("") { "%02x".format(it) }
    }
}
