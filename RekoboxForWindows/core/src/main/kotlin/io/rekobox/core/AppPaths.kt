package io.rekobox.core

import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths

/**
 * 统一的路径解析。
 *
 * 数据目录：%LOCALAPPDATA%\Rekobox
 * 内核二进制查找顺序：
 *   1. 环境变量 REKOBOX_CORE
 *   2. <安装目录>\sing-box.exe        （jpackage 打包后，与 Rekobox.exe 同级）
 *   3. <cwd>\runtime\windows\sing-box.exe  （开发期）
 *   4. PATH 中的 sing-box
 */
object AppPaths {

    val dataDir: Path by lazy {
        val base = System.getenv("LOCALAPPDATA")?.takeIf { it.isNotBlank() }
            ?: System.getProperty("user.home")
        Paths.get(base, "Rekobox").also { Files.createDirectories(it) }
    }

    val configFile: Path get() = dataDir.resolve("config.json")
    val stateFile: Path get() = dataDir.resolve("state.json")
    val logDir: Path get() = dataDir.resolve("logs").also { Files.createDirectories(it) }
    val profileDir: Path get() = dataDir.resolve("profiles").also { Files.createDirectories(it) }

    fun findCoreBinary(): Path? {
        System.getenv("REKOBOX_CORE")?.takeIf { it.isNotBlank() }?.let {
            val p = Paths.get(it)
            if (Files.isExecutable(p)) return p
        }

        val cwd = Paths.get(System.getProperty("user.dir"))
        val candidates = buildList {
            add(appDir().resolve("sing-box.exe"))
            // jpackage 把 appResourcesRootDir/windows 下的文件放进 <安装目录>/app/resources
            add(appDir().resolve("resources/sing-box.exe"))
            appDir().parent?.let { add(it.resolve("sing-box.exe")) }
            appDir().parent?.let { add(it.resolve("resources/sing-box.exe")) }
            add(cwd.resolve("sing-box.exe"))
            // 开发期 cwd 可能是 app/ 或 core/，向上找几级直到仓库里的 runtime/windows
            var dir: Path? = cwd
            repeat(4) {
                dir?.let { d ->
                    add(d.resolve("runtime/windows/sing-box.exe"))
                    add(d.resolve("RekoboxForWindows/runtime/windows/sing-box.exe"))
                }
                dir = dir?.parent
            }
        }
        candidates.firstOrNull { Files.isExecutable(it) }?.let { return it }

        return runCatching {
            ProcessBuilder("where", "sing-box.exe")
                .redirectErrorStream(true).start()
                .inputStream.bufferedReader().readLine()
                ?.trim()?.takeIf { it.isNotEmpty() }?.let { Paths.get(it) }
        }.getOrNull()
    }

    /** jpackage 打包后 jar 所在目录（<install>/app）；开发期退化为 cwd */
    private fun appDir(): Path {
        val loc = AppPaths::class.java.protectionDomain?.codeSource?.location ?: return Paths.get(".")
        return runCatching { Paths.get(loc.toURI()).parent ?: Paths.get(".") }.getOrDefault(Paths.get("."))
    }
}
