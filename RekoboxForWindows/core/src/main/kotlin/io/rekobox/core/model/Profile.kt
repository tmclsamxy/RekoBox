package io.rekobox.core.model

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import java.util.UUID

/**
 * 一个节点。
 *
 * Android 版把它存在 Room 里（`ProxyEntity` + `AbstractBean`）；
 * Windows 版只保留 Windows 需要的字段，outbound 直接以 sing-box 原生 JSON 片段保存，
 * 这样新增协议不需要动数据表 —— 解析器产出什么，内核就吃什么。
 */
@Serializable
data class Profile(
    val id: String = UUID.randomUUID().toString(),
    /** 显示名，来自链接的 #fragment 或订阅备注 */
    val name: String,
    /** sing-box outbound 类型：shadowsocks / vmess / vless / trojan / hysteria2 / tuic / anytls / socks / http */
    val type: String,
    val server: String = "",
    val port: Int = 0,
    /** sing-box outbound JSON（不含 tag，tag 由 id 派生，保证唯一） */
    val outbound: JsonObject,
    /** 来源订阅 id；手填节点为 null */
    val sourceId: String? = null,
    /** 订阅里的原始链接，便于导出与排查 */
    val rawLink: String = "",
    val enabled: Boolean = true,
) {
    /** sing-box 里的唯一 tag，同时用作 Clash API 里的节点名 */
    val tag: String get() = "node-" + id.take(8)

    val endpoint: String get() = if (port > 0) "$server:$port" else server
}

/** 一个订阅源 */
@Serializable
data class Subscription(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val url: String,
    /** 上次拉取时间（epoch millis） */
    val lastUpdate: Long = 0,
    val lastCount: Int = 0,
    val lastError: String? = null,
    /** 是否参与「全部更新」 */
    val enabled: Boolean = true,
)

/** 运行模式 */
@Serializable
enum class RunMode {
    /** TUN 接管全局流量，需要管理员 */
    TUN,

    /** 只开混合入站 + 写 WinINet 系统代理，无需管理员 */
    SYSTEM_PROXY,

    /** 只开混合入站，不动系统设置（给浏览器插件 / 其他程序手动指向） */
    MANUAL,
}

/** 可在界面里调整、会持久化的设置 */
@Serializable
data class EngineSettings(
    val mixedPort: Int = 2080,
    val clashPort: Int = 9090,
    val runMode: RunMode = RunMode.TUN,
    val remoteDns: String = "https://1.1.1.1/dns-query",
    val directDns: String = "223.5.5.5",
    val logLevel: String = "info",
    /** 局域网 / 私有地址直连 */
    val bypassLan: Boolean = true,
    /** 开启后由 sing-box 自动选择可用节点（selector 之外再加 url-test 组） */
    val autoSelect: Boolean = false,
    /** 自动选择的测速间隔（秒） */
    val urlTestIntervalSec: Int = 300,
    /** 启动后最小化到托盘 */
    val startMinimized: Boolean = false,
    /** 关闭窗口时最小化到托盘而不是退出 */
    val closeToTray: Boolean = true,
    /** 开机自启 */
    val autostart: Boolean = false,
)

/** 落盘的完整状态 */
@Serializable
data class SavedState(
    val profiles: List<Profile> = emptyList(),
    val subscriptions: List<Subscription> = emptyList(),
    val selectedId: String? = null,
    val settings: EngineSettings = EngineSettings(),
)
