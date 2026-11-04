package io.rekobox.core.store

import io.rekobox.core.AppPaths
import io.rekobox.core.model.Profile
import io.rekobox.core.model.SavedState
import io.rekobox.core.model.Subscription
import kotlinx.serialization.json.Json
import java.nio.file.Files
import java.nio.file.Path

/**
 * 节点库 / 订阅 / 设置的落盘。
 *
 * Android 用 Room（`ProxyEntity`、`AssetEntity`…）+ DataStore；Windows 版本数据量小
 * （几百个节点顶天），一个 JSON 文件足够，也方便用户手工编辑和跟 Android 备份互转。
 */
class AppStore(val file: Path = AppPaths.stateFile) {

    private val json = Json {
        prettyPrint = true
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    var state: SavedState = SavedState()
        private set

    fun load(): SavedState {
        state = runCatching {
            if (Files.exists(file)) json.decodeFromString(SavedState.serializer(), Files.readString(file))
            else SavedState()
        }.getOrDefault(SavedState())
        return state
    }

    fun save() {
        Files.createDirectories(file.parent)
        val tmp = file.resolveSibling("${file.fileName}.tmp")
        Files.writeString(tmp, json.encodeToString(SavedState.serializer(), state))
        Files.move(tmp, file, java.nio.file.StandardCopyOption.REPLACE_EXISTING)
    }

    // ------------------------------------------------------------ 节点

    /** 同一订阅重复拉取时按 rawLink 去重，手填节点按 id 区分 */
    fun mergeProfiles(incoming: List<Profile>, sourceId: String?): Int {
        val existing = state.profiles.toMutableList()
        var added = 0
        incoming.forEach { p ->
            val dup = existing.any { it.rawLink == p.rawLink && it.rawLink.isNotEmpty() }
            if (!dup) {
                existing += p.copy(sourceId = sourceId)
                added++
            }
        }
        state = state.copy(profiles = existing)
        if (state.selectedId == null) state = state.copy(selectedId = existing.firstOrNull()?.id)
        save()
        return added
    }

    fun replaceSubscriptionProfiles(sourceId: String, incoming: List<Profile>): Int {
        val kept = state.profiles.filter { it.sourceId != sourceId }
        val merged = kept + incoming.map { it.copy(sourceId = sourceId) }
        state = state.copy(profiles = merged)
        if (state.profiles.none { it.id == state.selectedId }) {
            state = state.copy(selectedId = merged.firstOrNull()?.id)
        }
        save()
        return merged.size - kept.size
    }

    fun removeProfile(id: String) {
        val remaining = state.profiles.filter { it.id != id }
        state = state.copy(
            profiles = remaining,
            selectedId = if (state.selectedId == id) remaining.firstOrNull()?.id else state.selectedId,
        )
        save()
    }

    fun setEnabled(id: String, enabled: Boolean) {
        state = state.copy(profiles = state.profiles.map { if (it.id == id) it.copy(enabled = enabled) else it })
        save()
    }

    fun renameProfile(id: String, name: String) {
        state = state.copy(profiles = state.profiles.map { if (it.id == id) it.copy(name = name) else it })
        save()
    }

    fun select(id: String?) {
        state = state.copy(selectedId = id)
        save()
    }

    fun selected(): Profile? = state.profiles.firstOrNull { it.id == state.selectedId }
        ?: state.profiles.firstOrNull { it.enabled }

    // ------------------------------------------------------------ 订阅

    fun upsertSubscription(sub: Subscription) {
        val list = state.subscriptions.toMutableList()
        val idx = list.indexOfFirst { it.id == sub.id }
        if (idx >= 0) list[idx] = sub else list += sub
        state = state.copy(subscriptions = list)
        save()
    }

    fun removeSubscription(id: String) {
        state = state.copy(
            subscriptions = state.subscriptions.filter { it.id != id },
            profiles = state.profiles.filter { it.sourceId != id },
        )
        if (state.profiles.none { it.id == state.selectedId }) {
            state = state.copy(selectedId = state.profiles.firstOrNull()?.id)
        }
        save()
    }

    // ------------------------------------------------------------ 设置

    fun updateSettings(block: (io.rekobox.core.model.EngineSettings) -> io.rekobox.core.model.EngineSettings) {
        state = state.copy(settings = block(state.settings))
        save()
    }
}
