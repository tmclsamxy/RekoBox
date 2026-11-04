package io.rekobox.win.platform

import java.nio.file.Paths

/**
 * 开机自启 + 管理员提权。
 *
 * TUN 模式必须管理员：Wintun 要创建虚拟网卡、改路由表。
 * 非管理员时只允许「系统代理」模式（mixed 入站 + WinINet 代理）。
 * 全部走 reg.exe / net.exe，不引第三方依赖。
 */
object StartupAndElevation {

    private const val RUN_KEY = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"

    fun isElevated(): Boolean = runCatching {
        ProcessBuilder("net", "session").redirectErrorStream(true).start().waitFor() == 0
    }.getOrDefault(false)

    /** 以管理员身份重启自己；成功后当前进程应主动退出 */
    fun relaunchElevated(): Boolean = runCatching {
        val info = ProcessHandle.current().info()
        val exe = info.command().orElse(null) ?: return false
        val args = info.arguments().orElse(emptyArray())
        val argList = args.joinToString(", ") { "'${it.replace("'", "''")}'" }
        val script = "Start-Process -FilePath '${exe.replace("'", "''")}' -ArgumentList @($argList) -Verb RunAs"
        ProcessBuilder("powershell", "-NoProfile", "-NonInteractive", "-Command", script)
            .redirectErrorStream(true).start().waitFor() == 0
    }.getOrDefault(false)

    fun isAutostartEnabled(): Boolean = runCatching {
        exec("reg", "query", RUN_KEY, "/v", "RekoBox").contains("RekoBox")
    }.getOrDefault(false)

    fun setAutostart(enable: Boolean, exePath: String = currentExe()): Boolean = runCatching {
        if (enable) {
            exec("reg", "add", RUN_KEY, "/v", "RekoBox", "/t", "REG_SZ", "/d", "\"$exePath\" --minimized", "/f")
        } else {
            exec("reg", "delete", RUN_KEY, "/v", "RekoBox", "/f")
        }
        true
    }.getOrDefault(false)

    private fun currentExe(): String =
        ProcessHandle.current().info().command().orElse(
            Paths.get(System.getProperty("java.home"), "bin", "java.exe").toString()
        )

    private fun exec(vararg cmd: String): String = runCatching {
        val p = ProcessBuilder(*cmd).redirectErrorStream(true).start()
        p.inputStream.bufferedReader().readText().also { p.waitFor() }
    }.getOrDefault("")
}
