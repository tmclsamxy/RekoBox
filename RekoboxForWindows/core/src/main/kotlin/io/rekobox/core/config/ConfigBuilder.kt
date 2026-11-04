package io.rekobox.core.config

import io.rekobox.core.AppPaths
import io.rekobox.core.model.EngineSettings
import io.rekobox.core.model.Profile
import io.rekobox.core.model.RunMode
import kotlinx.serialization.json.add
import kotlinx.serialization.json.addJsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject

/**
 * 生成 sing-box 配置。
 *
 * 与 Android 版的差异：Android 由 Kotlin 组装 config 字符串再交给 libcore 的
 * `NewSingBoxInstance`；这里是标准 sing-box JSON 文件，交给独立进程读取。
 *
 * 节点全部进 `proxy` selector，Clash API 才能列出并切换它们 —— 这也是 Windows 版
 * 唯一的分流入口（Android 靠 AIDL 直连核心，不需要这层）。
 */
object ConfigBuilder {

    const val GROUP_SELECT = "proxy"
    const val GROUP_AUTO = "auto"
    const val OUTBOUND_DIRECT = "direct"
    const val OUTBOUND_BLOCK = "block"
    private const val TEST_URL = "https://www.gstatic.com/generate_204"

    fun build(
        profiles: List<Profile>,
        selectedTag: String?,
        s: EngineSettings,
    ): String {
        val active = profiles.filter { it.enabled }
        require(active.isNotEmpty()) { "没有可用节点" }
        val tags = active.map { it.tag }
        val default = selectedTag?.takeIf { it in tags } ?: tags.first()

        return buildJsonObject {
            putJsonObject("log") {
                put("level", s.logLevel)
                put("timestamp", true)
            }

            putJsonObject("dns") {
                putJsonArray("servers") {
                    addJsonObject {
                        put("tag", "dns-remote")
                        put("address", s.remoteDns)
                        put("detour", GROUP_SELECT)
                    }
                    addJsonObject {
                        put("tag", "dns-direct")
                        put("address", s.directDns)
                        put("detour", OUTBOUND_DIRECT)
                    }
                }
                put("final", "dns-remote")
                put("strategy", "prefer_ipv4")
                put("independent_cache", true)
            }

            putJsonArray("inbounds") {
                if (s.runMode == RunMode.TUN) {
                    addJsonObject {
                        put("type", "tun")
                        put("tag", "tun-in")
                        putJsonArray("address") { add("172.19.0.1/30") }
                        put("mtu", 9000)
                        put("auto_route", true)
                        put("strict_route", true)
                        put("stack", "gvisor")
                    }
                }
                // 混合入站常开：系统代理模式靠它；TUN 模式下也方便单进程指过来
                addJsonObject {
                    put("type", "mixed")
                    put("tag", "mixed-in")
                    put("listen", "127.0.0.1")
                    put("listen_port", s.mixedPort)
                }
            }

            putJsonArray("outbounds") {
                // 手动选择组
                addJsonObject {
                    put("type", "selector")
                    put("tag", GROUP_SELECT)
                    putJsonArray("outbounds") {
                        if (s.autoSelect) add(GROUP_AUTO)
                        tags.forEach { add(it) }
                        add(OUTBOUND_DIRECT)
                    }
                    put("default", if (s.autoSelect) GROUP_AUTO else default)
                }
                // 自动测速组
                if (s.autoSelect) {
                    addJsonObject {
                        put("type", "urltest")
                        put("tag", GROUP_AUTO)
                        putJsonArray("outbounds") { tags.forEach { add(it) } }
                        put("url", TEST_URL)
                        put("interval", "${s.urlTestIntervalSec}s")
                        put("tolerance", 50)
                    }
                }
                active.forEach { p ->
                    addJsonObject {
                        p.outbound.forEach { (k, v) -> put(k, v) }
                        put("tag", p.tag)
                    }
                }
                addJsonObject {
                    put("type", OUTBOUND_DIRECT)
                    put("tag", OUTBOUND_DIRECT)
                }
                addJsonObject {
                    put("type", "block")
                    put("tag", OUTBOUND_BLOCK)
                }
            }

            putJsonObject("route") {
                putJsonArray("rules") {
                    if (s.bypassLan) {
                        addJsonObject {
                            put("ip_is_private", true)
                            put("outbound", OUTBOUND_DIRECT)
                        }
                    }
                    // Clash API 自身流量必须直连，否则测速会绕进代理
                    addJsonObject {
                        putJsonArray("port") { add(s.clashPort) }
                        put("outbound", OUTBOUND_DIRECT)
                    }
                }
                put("final", GROUP_SELECT)
                put("auto_detect_interface", true)
            }

            putJsonObject("experimental") {
                putJsonObject("clash_api") {
                    put("external_controller", "127.0.0.1:${s.clashPort}")
                }
                putJsonObject("cache_file") {
                    put("enabled", true)
                    put("path", AppPaths.dataDir.resolve("cache.db").toString())
                }
            }
        }.toString()
    }
}
