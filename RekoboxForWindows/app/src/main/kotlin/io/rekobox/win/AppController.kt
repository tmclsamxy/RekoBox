package io.rekobox.win

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import io.rekobox.core.AppPaths
import io.rekobox.core.clash.ClashApi
import io.rekobox.core.config.ConfigBuilder
import io.rekobox.core.engine.EngineState
import io.rekobox.core.engine.SingBoxEngine
import io.rekobox.core.model.EngineSettings
import io.rekobox.core.model.Profile
import io.rekobox.core.model.RunMode
import io.rekobox.core.model.Subscription
import io.rekobox.core.profile.ProfileParser
import io.rekobox.core.store.AppStore
import io.rekobox.core.subscription.SubscriptionFetcher
import io.rekobox.win.platform.StartupAndElevation
import io.rekobox.win.platform.SystemProxy
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * 界面与内核之间的唯一中介。
 *
 * 所有状态都是 Compose 的 mutableState，UI 直接读；耗时操作一律丢到 scope 里执行，
 * 保证界面线程不碰进程管理和注册表。
 */
class AppController(private val scope: CoroutineScope) {

    val store = AppStore()

    var saved by mutableStateOf(store.load())
        private set

    val profiles: List<Profile> get() = saved.profiles
    val subscriptions: List<Subscription> get() = saved.subscriptions
    val settings: EngineSettings get() = saved.settings

    val corePath = AppPaths.findCoreBinary()
    val engine: SingBoxEngine? = corePath?.let { runCatching { SingBoxEngine(it) }.getOrNull() }
    val clash: ClashApi get() = ClashApi("127.0.0.1:${settings.clashPort}")

    var running by mutableStateOf(false)
        private set
    var statusText by mutableStateOf(if (corePath == null) "未找到 sing-box 内核" else "已停止")
        private set
    var up by mutableStateOf(0L)
        private set
    var down by mutableStateOf(0L)
        private set
    var delays by mutableStateOf<Map<String, Int?>>(emptyMap())
        private set
    var testingAll by mutableStateOf(false)
        private set
    var activeMode by mutableStateOf(RunMode.TUN)
        private set
    var connections by mutableStateOf<List<io.rekobox.core.clash.ConnInfo>>(emptyList())
        private set
    var message by mutableStateOf<String?>(null)

    // ---- 界面状态（菜单栏和主界面共用，放在这里省一层状态传递）----
    var sheet by mutableStateOf(Sheet.NONE)
    var logsVisible by mutableStateOf(true)
    /** null = 全部；"ungrouped" = 手填节点；其他 = 订阅 id */
    var filterSource by mutableStateOf<String?>(null)

    val logs = mutableStateListOf<String>()

    private var trafficJob: Job? = null
    private var connJob: Job? = null
    private var systemProxyApplied = false
    private val fetcher = SubscriptionFetcher()

    init {
        if (corePath == null) {
            addLog("[内核] 未找到 sing-box.exe。运行 scripts/fetch-core.ps1 下载，或设置环境变量 REKOBOX_CORE")
        } else {
            addLog("[内核] 使用 ${corePath.toAbsolutePath()}")
            scope.launch(Dispatchers.IO) {
                addLog("[内核] 版本 ${engine?.coreVersion() ?: "未知"}")
            }
        }
        engine?.let { e ->
            scope.launch { e.logs.collect { line -> addLog(line) } }
        }
    }

    fun addLog(line: String) {
        logs.add("${TIME.format(Instant.now())}  $line")
        while (logs.size > 1000) logs.removeAt(0)
    }

    // ------------------------------------------------------------ 启停

    fun toggleElevation(onRelaunch: () -> Unit) {
        if (running) stop() else start(onRelaunch)
    }

    fun start(onRelaunch: () -> Unit = {}) {
        val e = engine ?: run { message = "未找到 sing-box 内核"; return }
        val selected = store.selected() ?: run { message = "先导入至少一个节点"; return }

        var mode = settings.runMode
        if (mode == RunMode.TUN && !StartupAndElevation.isElevated()) {
            addLog("[系统] TUN 模式需要管理员权限，正在申请提权…")
            if (StartupAndElevation.relaunchElevated()) {
                onRelaunch()
                return
            }
            addLog("[系统] 提权被拒绝，本次回退到系统代理模式")
            message = "提权被拒绝，已回退到系统代理模式"
            mode = RunMode.SYSTEM_PROXY
        }

        val config = runCatching { ConfigBuilder.build(profiles, selected.tag, settings.copy(runMode = mode)) }
            .getOrElse {
                message = it.message
                return
            }

        scope.launch {
            statusText = "启动中…"
            e.start(config)
            running = e.isRunning()
            activeMode = mode
            statusText = if (running) "运行中 · ${selected.name}" else "启动失败"
            if (running) {
                if (mode == RunMode.SYSTEM_PROXY) {
                    withContext(Dispatchers.IO) { SystemProxy.enable(port = settings.mixedPort) }
                    systemProxyApplied = true
                    addLog("[系统] 系统代理 → 127.0.0.1:${settings.mixedPort}")
                }
                watchTraffic()
                watchConnections()
                addLog("[界面] 已启动，模式 ${mode.label()}")
            } else {
                message = "内核启动失败，看日志"
            }
        }
    }

    fun stop() {
        scope.launch {
            trafficJob?.cancel()
            trafficJob = null
            connJob?.cancel()
            connJob = null
            connections = emptyList()
            engine?.stop()
            if (systemProxyApplied) {
                withContext(Dispatchers.IO) { SystemProxy.disable() }
                systemProxyApplied = false
                addLog("[系统] 系统代理已还原")
            }
            running = false
            statusText = "已停止"
            up = 0
            down = 0
        }
    }

    /** 进程退出 / 关窗口时的兜底：内核和系统代理都不能留 */
    fun shutdown() {
        runCatching { engine?.let { e -> kotlinx.coroutines.runBlocking { e.stop() } } }
        if (systemProxyApplied) runCatching { SystemProxy.disable() }
        systemProxyApplied = false
    }

    private fun watchConnections() {
        connJob?.cancel()
        connJob = scope.launch {
            while (true) {
                connections = withContext(Dispatchers.IO) {
                    runCatching { clash.connectionList() }.getOrDefault(emptyList())
                }
                kotlinx.coroutines.delay(2000)
            }
        }
    }

    private fun watchTraffic() {
        trafficJob?.cancel()
        trafficJob = scope.launch {
            while (true) {
                runCatching { clash.traffic().collect { up = it.upload; down = it.download } }
                kotlinx.coroutines.delay(1000)
            }
        }
    }

    // ------------------------------------------------------------ 节点

    fun importText(text: String) {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) return
        val result = ProfileParser.parseSubscription(trimmed)
        if (result.profiles.isEmpty()) {
            message = "没有解析出节点：${result.errors.firstOrNull() ?: "格式不认识"}"
            return
        }
        val added = store.mergeProfiles(result.profiles, null)
        reload()
        message = if (result.errors.isEmpty()) "导入 $added 个节点" else "导入 $added 个节点，${result.errors.size} 行跳过"
        addLog("[节点] 导入 ${result.profiles.size} 个，新增 $added 个")
    }

    fun removeProfile(id: String) {
        store.removeProfile(id)
        reload()
    }

    /** 「服务器 → 从剪贴板添加」，与 NekoRay 的同名菜单对齐 */
    fun importFromClipboard() {
        val text = runCatching {
            java.awt.Toolkit.getDefaultToolkit().systemClipboard
                .getData(java.awt.datatransfer.DataFlavor.stringFlavor) as? String
        }.getOrNull()
        if (text.isNullOrBlank()) {
            message = "剪贴板里没有文本"
            return
        }
        importText(text)
    }

    fun copyLink(p: Profile) {
        runCatching {
            java.awt.Toolkit.getDefaultToolkit().systemClipboard
                .setContents(java.awt.datatransfer.StringSelection(p.rawLink), null)
        }
        message = "已复制 ${p.name} 的链接"
    }

    fun setEnabled(id: String, enabled: Boolean) {
        store.setEnabled(id, enabled)
        reload()
    }

    fun rename(id: String, name: String) {
        store.renameProfile(id, name.ifBlank { "未命名" })
        reload()
    }

    fun select(profile: Profile) {
        store.select(profile.id)
        reload()
        if (running) {
            scope.launch(Dispatchers.IO) {
                val ok = clash.select(ConfigBuilder.GROUP_SELECT, profile.tag)
                scope.launch { addLog("[节点] 切换到 ${profile.name} ${if (ok) "成功" else "失败"}") }
            }
        }
    }

    fun testDelay(profile: Profile) {
        if (!running) {
            message = "先启动内核再测延迟"
            return
        }
        scope.launch(Dispatchers.IO) {
            val ms = clash.delay(profile.tag)
            scope.launch { delays = delays + (profile.id to ms) }
        }
    }

    fun testAllDelays() {
        if (!running) {
            message = "先启动内核再测延迟"
            return
        }
        testingAll = true
        scope.launch(Dispatchers.IO) {
            profiles.filter { it.enabled }.forEach { p ->
                val ms = clash.delay(p.tag)
                scope.launch { delays = delays + (p.id to ms) }
            }
            scope.launch { testingAll = false }
        }
    }

    // ------------------------------------------------------------ 订阅

    fun addSubscription(name: String, url: String) {
        val trimmed = url.trim()
        if (trimmed.isEmpty()) return
        val sub = Subscription(name = name.ifBlank { "订阅 ${subscriptions.size + 1}" }, url = trimmed)
        store.upsertSubscription(sub)
        reload()
        refreshSubscription(sub)
    }

    fun refreshSubscription(sub: Subscription) {
        scope.launch {
            message = "更新订阅 ${sub.name}…"
            val result = runCatching { withContext(Dispatchers.IO) { fetcher.fetch(sub.url) } }
            result.onSuccess { r ->
                if (r.profiles.isEmpty()) {
                    store.upsertSubscription(sub.copy(lastError = r.errors.firstOrNull() ?: "空订阅", lastUpdate = now()))
                    reload()
                    message = "订阅 ${sub.name} 没有可用节点"
                    return@onSuccess
                }
                store.replaceSubscriptionProfiles(sub.id, r.profiles)
                store.upsertSubscription(
                    sub.copy(lastUpdate = now(), lastCount = r.profiles.size, lastError = r.errors.firstOrNull())
                )
                reload()
                message = "订阅 ${sub.name} 更新完成，${r.profiles.size} 个节点"
                addLog("[订阅] ${sub.name} → ${r.profiles.size} 个节点")
            }.onFailure { e ->
                store.upsertSubscription(sub.copy(lastError = e.message ?: "更新失败"))
                reload()
                message = "订阅更新失败：${e.message}"
            }
        }
    }

    fun refreshAllSubscriptions() {
        subscriptions.filter { it.enabled }.forEach { refreshSubscription(it) }
    }

    fun removeSubscription(sub: Subscription) {
        store.removeSubscription(sub.id)
        reload()
    }

    // ------------------------------------------------------------ 设置

    fun updateSettings(new: EngineSettings) {
        val needRestart = running && (
            new.runMode != settings.runMode ||
                new.mixedPort != settings.mixedPort ||
                new.clashPort != settings.clashPort ||
                new.autoSelect != settings.autoSelect
            )
        if (new.autostart != settings.autostart) {
            StartupAndElevation.setAutostart(new.autostart)
        }
        store.updateSettings { new }
        reload()
        if (needRestart) {
            message = "设置已保存，正在重启内核…"
            stop()
            scope.launch {
                kotlinx.coroutines.delay(600)
                start()
            }
        }
    }

    fun reload() {
        saved = store.state
    }

    fun clearDelays() {
        delays = emptyMap()
    }

    private fun now(): Long = System.currentTimeMillis()

    companion object {
        private val TIME: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss").withZone(ZoneId.systemDefault())
    }
}

/** 主界面上的浮层（参照 NekoRay 的对话框式交互） */
enum class Sheet { NONE, IMPORT, SUBSCRIPTIONS, SETTINGS, ABOUT }

fun RunMode.label(): String = when (this) {    RunMode.TUN -> "TUN"
    RunMode.SYSTEM_PROXY -> "系统代理"
    RunMode.MANUAL -> "仅本地代理"
}

/** 引擎状态 → 界面文案（扩展点，便于以后加更多状态） */
fun EngineState.label(): String = when (this) {
    EngineState.STOPPED -> "已停止"
    EngineState.STARTING -> "启动中"
    EngineState.RUNNING -> "运行中"
    EngineState.STOPPING -> "停止中"
    EngineState.FAILED -> "失败"
}
