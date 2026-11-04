package io.rekobox.win.platform

import io.rekobox.core.AppPaths
import java.nio.file.Files
import java.util.Properties

/**
 * Windows 系统代理（WinINet）。
 *
 * Android 版是全局 VPN，没有「系统代理」概念；Windows 上非管理员模式只能改这里 ——
 * 影响浏览器、Edge/Chrome，以及所有走 WinINet 的桌面程序。走裸 socket 的程序不受影响，
 * 要覆盖它们必须用 TUN。
 *
 * 关键点：开之前先备份用户原有设置，退出时原样还原，避免把别人的代理配置吃掉。
 */
object SystemProxy {

    private const val KEY = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"
    private const val BYPASS = "localhost;127.*;10.*;172.16.*;172.17.*;172.18.*;172.19.*;172.20.*;172.21.*;172.22.*;172.23.*;172.24.*;172.25.*;172.26.*;172.27.*;172.28.*;172.29.*;172.30.*;172.31.*;192.168.*;<local>"

    private val backupFile get() = AppPaths.dataDir.resolve("system-proxy-backup.properties")

    fun isEnabled(): Boolean = read("ProxyEnable")?.contains("0x1") == true

    fun enable(host: String = "127.0.0.1", port: Int) {
        backup()
        write("ProxyEnable", "REG_DWORD", "1")
        write("ProxyServer", "REG_SZ", "$host:$port")
        write("ProxyOverride", "REG_SZ", BYPASS)
        refresh()
    }

    /** 还原到开启前的状态；没有备份就只是关掉 */
    fun disable() {
        val props = loadBackup()
        if (props == null) {
            write("ProxyEnable", "REG_DWORD", "0")
        } else {
            write("ProxyEnable", "REG_DWORD", props.getProperty("ProxyEnable", "0").removePrefix("0x").let {
                if (it == "1") "1" else "0"
            })
            props.getProperty("ProxyServer")?.let { write("ProxyServer", "REG_SZ", it) }
            props.getProperty("ProxyOverride")?.let { write("ProxyOverride", "REG_SZ", it) }
        }
        refresh()
    }

    private fun backup() {
        val props = Properties()
        props["ProxyEnable"] = read("ProxyEnable")?.let { if (it.contains("0x1")) "1" else "0" } ?: "0"
        read("ProxyServer")?.let { props["ProxyServer"] = it }
        read("ProxyOverride")?.let { props["ProxyOverride"] = it }
        Files.createDirectories(AppPaths.dataDir)
        Files.newOutputStream(backupFile).use { props.store(it, "saved by Rekobox before enabling system proxy") }
    }

    private fun loadBackup(): Properties? = runCatching {
        if (!Files.exists(backupFile)) return null
        Properties().apply { Files.newInputStream(backupFile).use { load(it) } }
    }.getOrNull()

    private fun read(name: String): String? {
        val out = reg("query", KEY, "/v", name)
        val line = out.lineSequence().firstOrNull { it.trim().startsWith(name) } ?: return null
        return line.substringAfter("REG_SZ", "").ifBlank { line.substringAfter("REG_DWORD", "") }
            .trim().ifBlank { null }
    }

    private fun write(name: String, type: String, value: String) {
        reg("add", KEY, "/v", name, "/t", type, "/d", value, "/f")
    }

    private fun reg(vararg args: String): String = runCatching {
        val p = ProcessBuilder(listOf("reg") + args).redirectErrorStream(true).start()
        p.inputStream.bufferedReader().readText().also { p.waitFor() }
    }.getOrDefault("")

    /** 改注册表后必须通知 WinINet，否则要重启浏览器才生效 */
    private fun refresh() {
        val script = """
            Add-Type -Namespace Rekobox -Name WinInet -MemberDefinition '[DllImport("wininet.dll", SetLastError = true)] public static extern bool InternetSetOption(IntPtr h, int o, IntPtr b, int l);'
            [Rekobox.WinInet]::InternetSetOption([IntPtr]::Zero, 39, [IntPtr]::Zero, 0) | Out-Null
            [Rekobox.WinInet]::InternetSetOption([IntPtr]::Zero, 37, [IntPtr]::Zero, 0) | Out-Null
        """.trimIndent()
        runCatching {
            ProcessBuilder(
                "powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script
            ).redirectErrorStream(true).start().waitFor()
        }
    }
}
