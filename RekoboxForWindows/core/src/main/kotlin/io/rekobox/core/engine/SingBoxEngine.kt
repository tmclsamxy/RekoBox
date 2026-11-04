package io.rekobox.core.engine

import io.rekobox.core.AppPaths
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit

enum class EngineState { STOPPED, STARTING, RUNNING, STOPPING, FAILED }

/**
 * sing-box 子进程管理器。
 *
 * 与 Android 版的根本差异：Android 由 App 创建 VpnService fd 再交给 Go 里的
 * tun2socks；Windows 上没人能替核心创建网卡，TUN 必须由 sing-box 自己
 * 用 Wintun 建立，因此这里只负责「写配置 → 起进程 → 收日志 → 优雅退出」。
 */
class SingBoxEngine(
    private val binary: Path = requireNotNull(AppPaths.findCoreBinary()) {
        "找不到 sing-box.exe，请运行 scripts/fetch-core.ps1 或设置环境变量 REKOBOX_CORE"
    },
) {

    private val _state = MutableStateFlow(EngineState.STOPPED)
    val state = _state.asStateFlow()

    private val _logs = MutableSharedFlow<String>(replay = 300, extraBufferCapacity = 512)
    val logs = _logs.asSharedFlow()

    private var process: Process? = null

    fun isRunning(): Boolean = process?.isAlive == true

    suspend fun start(configJson: String) = withContext(Dispatchers.IO) {
        if (isRunning()) return@withContext
        _state.value = EngineState.STARTING

        Files.writeString(AppPaths.configFile, configJson)

        val pb = ProcessBuilder(binary.toAbsolutePath().toString(), "run", "-c", AppPaths.configFile.toString())
            .redirectErrorStream(true)
            .directory(AppPaths.dataDir.toFile())

        val proc = try {
            pb.start()
        } catch (e: Exception) {
            _state.value = EngineState.FAILED
            _logs.tryEmit("[engine] 启动失败: ${e.message}")
            return@withContext
        }
        process = proc

        // 日志泵
        CoroutineScopeHolder.scope.launch(Dispatchers.IO) {
            proc.inputStream.bufferedReader().useLines { lines ->
                lines.forEach { line ->
                    _logs.tryEmit(line)
                    if (line.contains("started", true) || line.contains("sing-box started", true)) {
                        _state.value = EngineState.RUNNING
                    }
                }
            }
            val code = proc.waitFor()
            process = null
            _state.value = if (code == 0) EngineState.STOPPED else EngineState.FAILED
            _logs.tryEmit("[engine] 进程退出，code=$code")
        }

        // 兜底：进程活着但没打印 started
        kotlinx.coroutines.delay(800)
        if (isRunning() && _state.value == EngineState.STARTING) {
            _state.value = EngineState.RUNNING
        }
    }

    /** 改端口 / 换运行模式后需要重载配置 */
    suspend fun restart(configJson: String) {
        stop()
        kotlinx.coroutines.delay(150)
        start(configJson)
    }

    suspend fun stop() = withContext(Dispatchers.IO) {        val proc = process ?: return@withContext
        _state.value = EngineState.STOPPING
        proc.destroy()
        if (!proc.waitFor(3, TimeUnit.SECONDS)) {
            proc.destroyForcibly()
            proc.waitFor(3, TimeUnit.SECONDS)
        }
        process = null
        _state.value = EngineState.STOPPED
    }

    fun coreVersion(): String = runCatching {
        ProcessBuilder(binary.toAbsolutePath().toString(), "version")
            .redirectErrorStream(true).start()
            .inputStream.bufferedReader().readText().trim().lineSequence().firstOrNull() ?: "unknown"
    }.getOrDefault("unknown")
}

internal object CoroutineScopeHolder {
    val scope = kotlinx.coroutines.CoroutineScope(
        kotlinx.coroutines.SupervisorJob() + Dispatchers.IO
    )
}
