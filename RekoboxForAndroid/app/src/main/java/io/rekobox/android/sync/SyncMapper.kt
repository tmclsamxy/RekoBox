package io.rekobox.android.sync

import io.rekobox.android.database.ProxyEntity
import io.rekobox.android.database.ProxyGroup
import io.rekobox.android.fmt.AbstractBean
import io.rekobox.android.fmt.http.HttpBean
import io.rekobox.android.fmt.shadowsocks.ShadowsocksBean
import io.rekobox.android.fmt.socks.SOCKSBean
import io.rekobox.android.fmt.trojan.TrojanBean
import io.rekobox.android.fmt.v2ray.StandardV2RayBean
import io.rekobox.android.fmt.v2ray.VMessBean
import org.json.JSONObject

/**
 * Android 实体 <-> Windows(nekoray) bean JSON 的互转。
 *
 * Windows 端的条目数据长这样（键很简写，来自 nekoray 的 JsonStore）：
 *   {"type":"vmess","bean":{"_v":0,"name":"HK","addr":"1.2.3.4","port":443,
 *                          "id":"<uuid>","aid":0,"sec":"auto",
 *                          "stream":{"net":"ws","sec":"tls","host":"a.com","path":"/x",...}}}
 *
 * 目前覆盖：socks / http / shadowsocks / vmess / vless / trojan —— 这几种在两端字段一一对应。
 * 其余类型（ssr、trojan-go、naive、hysteria、tuic、wireguard、ssh、chain、config…）
 * 返回 null，由上层当作「看不懂的条目」原样透传，绝不改写、绝不删除。
 */
object SyncMapper {

    private const val NKR_HTTP_TYPE = -80
    private const val NKR_SOCKS5 = 5
    private const val NKR_SOCKS4 = 4

    // ---------- 导出 ----------

    /** 返回 null 表示本端不支持该类型，交给 foreign 透传 */
    fun exportProfile(entity: ProxyEntity): Pair<String, JSONObject>? {
        val bean = try {
            entity.requireBean()
        } catch (_: Exception) {
            return null
        }
        return when (bean) {
            is VMessBean -> {
                val typeName = if ((bean.alterId ?: 0) == -1) "vless" else "vmess"
                typeName to v2rayToJson(bean, typeName)
            }

            is TrojanBean -> "trojan" to v2rayToJson(bean, "trojan")
            is HttpBean -> "http" to httpToJson(bean)
            is SOCKSBean -> "socks" to socksToJson(bean)
            is ShadowsocksBean -> "shadowsocks" to ssToJson(bean)
            else -> null
        }
    }

    fun exportGroup(group: ProxyGroup): JSONObject {
        val o = JSONObject()
        group.name?.takeIf { it.isNotEmpty() }?.let { o.put("name", it) }
        // 订阅地址只在有值时导出：缺省键在 Windows 侧不会覆盖原有 url
        group.subscription?.link?.takeIf { it.isNotEmpty() }?.let { o.put("url", it) }
        return o
    }

    private fun base(bean: AbstractBean): JSONObject = JSONObject().apply {
        put("_v", 0)
        bean.name?.takeIf { it.isNotEmpty() }?.let { put("name", it) }
        bean.serverAddress?.takeIf { it.isNotEmpty() }?.let { put("addr", it) }
        put("port", bean.serverPort ?: 0)
    }

    private fun v2rayToJson(bean: StandardV2RayBean, typeName: String): JSONObject {
        val o = base(bean)
        when (typeName) {
            "vmess" -> {
                bean.uuid?.takeIf { it.isNotEmpty() }?.let { o.put("id", it) }
                o.put("aid", (bean as? VMessBean)?.alterId ?: 0)
                bean.encryption?.takeIf { it.isNotEmpty() }?.let { o.put("sec", it) }
            }

            "vless" -> {
                bean.uuid?.takeIf { it.isNotEmpty() }?.let { o.put("pass", it) }
                bean.encryption?.takeIf { it.isNotEmpty() }?.let { o.put("flow", it) }
            }

            "trojan" -> {
                (bean as? TrojanBean)?.password?.takeIf { it.isNotEmpty() }?.let { o.put("pass", it) }
            }
        }
        o.put("stream", streamToJson(bean))
        return o
    }

    private fun streamToJson(bean: StandardV2RayBean): JSONObject {
        val s = JSONObject()
        // Android 的 "http" 就是 HTTP/2，nekoray 里叫 h2
        val net = when (val t = bean.type?.lowercase() ?: "tcp") {
            "http" -> "h2"
            else -> t
        }
        s.put("net", net)
        bean.security?.takeIf { it.isNotEmpty() && it != "none" }?.let { s.put("sec", it) }
        bean.host?.takeIf { it.isNotEmpty() }?.let { s.put("host", it) }
        bean.path?.takeIf { it.isNotEmpty() }?.let { s.put("path", it) }
        bean.sni?.takeIf { it.isNotEmpty() }?.let { s.put("sni", it) }
        bean.alpn?.takeIf { it.isNotEmpty() }?.let { s.put("alpn", it) }
        if (bean.allowInsecure == true) s.put("insecure", true)
        bean.headerType?.takeIf { it.isNotEmpty() && it != "none" }?.let { s.put("h_type", it) }
        bean.earlyDataHeaderName?.takeIf { it.isNotEmpty() }?.let { s.put("ed_name", it) }
        (bean.wsMaxEarlyData ?: 0).takeIf { it > 0 }?.let { s.put("ed_len", it) }
        bean.utlsFingerprint?.takeIf { it.isNotEmpty() }?.let { s.put("utls", it) }
        bean.realityPubKey?.takeIf { it.isNotEmpty() }?.let { s.put("pbk", it) }
        bean.realityShortId?.takeIf { it.isNotEmpty() }?.let { s.put("sid", it) }
        (bean.packetEncoding ?: 0).takeIf { it > 0 }?.let { s.put("pac_enc", it) }
        if (bean.enableMux == true) s.put("mux_s", 1)
        return s
    }

    private fun httpToJson(bean: HttpBean): JSONObject {
        val o = base(bean)
        o.put("v", NKR_HTTP_TYPE)
        bean.username?.takeIf { it.isNotEmpty() }?.let { o.put("username", it) }
        bean.password?.takeIf { it.isNotEmpty() }?.let { o.put("password", it) }
        o.put("stream", streamToJson(bean))
        return o
    }

    private fun socksToJson(bean: SOCKSBean): JSONObject {
        val o = base(bean)
        o.put(
            "v", when (bean.protocol) {
                SOCKSBean.PROTOCOL_SOCKS4 -> NKR_SOCKS4
                else -> NKR_SOCKS5
            }
        )
        bean.username?.takeIf { it.isNotEmpty() }?.let { o.put("username", it) }
        bean.password?.takeIf { it.isNotEmpty() }?.let { o.put("password", it) }
        return o
    }

    private fun ssToJson(bean: ShadowsocksBean): JSONObject {
        val o = base(bean)
        bean.method?.takeIf { it.isNotEmpty() }?.let { o.put("method", it) }
        bean.password?.takeIf { it.isNotEmpty() }?.let { o.put("pass", it) }
        bean.plugin?.takeIf { it.isNotEmpty() }?.let { o.put("plugin", it) }
        return o
    }

    // ---------- 导入 ----------

    /** 由 Windows 条目数据构造 Android bean；不支持的类型返回 null */
    fun importBean(data: JSONObject): AbstractBean? {
        val typeName = data.optString("type")
        val beanJson = data.optJSONObject("bean") ?: JSONObject()
        return try {
            when (typeName) {
                "vmess", "vless" -> VMessBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.uuid = if (typeName == "vless") beanJson.optString("pass") else beanJson.optString("id")
                    bean.alterId = if (typeName == "vless") -1 else beanJson.optInt("aid", 0)
                    bean.encryption = if (typeName == "vless") beanJson.optString("flow")
                    else beanJson.optString("sec", "auto")
                    applyStream(bean, beanJson.optJSONObject("stream"))
                }

                "trojan" -> TrojanBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.password = beanJson.optString("pass")
                    applyStream(bean, beanJson.optJSONObject("stream"))
                    if (bean.security.isNullOrEmpty() || bean.security == "none") bean.security = "tls"
                }

                "http" -> HttpBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.username = beanJson.optString("username")
                    bean.password = beanJson.optString("password")
                    applyStream(bean, beanJson.optJSONObject("stream"))
                }

                "socks" -> SOCKSBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.protocol = when (beanJson.optInt("v", NKR_SOCKS5)) {
                        NKR_SOCKS4 -> SOCKSBean.PROTOCOL_SOCKS4
                        else -> SOCKSBean.PROTOCOL_SOCKS5
                    }
                    bean.username = beanJson.optString("username")
                    bean.password = beanJson.optString("password")
                }

                "shadowsocks" -> ShadowsocksBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.method = beanJson.optString("method", "aes-128-gcm")
                    bean.password = beanJson.optString("pass")
                    bean.plugin = beanJson.optString("plugin")
                }

                else -> null
            }
        } catch (_: Exception) {
            null
        }
    }

    fun applyGroup(group: ProxyGroup, data: JSONObject) {
        data.optString("name").takeIf { it.isNotEmpty() }?.let { group.name = it }
    }

    private fun applyBase(bean: AbstractBean, o: JSONObject) {
        bean.name = o.optString("name")
        o.optString("addr").takeIf { it.isNotEmpty() }?.let { bean.serverAddress = it }
        bean.serverPort = o.optInt("port", 1080)
        o.optString("c_out").takeIf { it.isNotEmpty() }?.let { bean.customOutboundJson = it }
        o.optString("c_cfg").takeIf { it.isNotEmpty() }?.let { bean.customConfigJson = it }
    }

    private fun applyStream(bean: StandardV2RayBean, s: JSONObject?) {
        if (s == null) return
        with(bean) {
            type = when (val net = s.optString("net", "tcp")) {
                "h2" -> "http"
                "" -> "tcp"
                else -> net
            }
            s.optString("sec").takeIf { it.isNotEmpty() }?.let { security = it }
            s.optString("host").takeIf { it.isNotEmpty() }?.let { host = it }
            s.optString("path").takeIf { it.isNotEmpty() }?.let { path = it }
            s.optString("sni").takeIf { it.isNotEmpty() }?.let { sni = it }
            s.optString("alpn").takeIf { it.isNotEmpty() }?.let { alpn = it }
            if (s.optBoolean("insecure")) allowInsecure = true
            s.optString("h_type").takeIf { it.isNotEmpty() }?.let { headerType = it }
            s.optString("ed_name").takeIf { it.isNotEmpty() }?.let { earlyDataHeaderName = it }
            val earlyData = s.optInt("ed_len", 0)
            if (earlyData > 0) wsMaxEarlyData = earlyData
            s.optString("utls").takeIf { it.isNotEmpty() }?.let { utlsFingerprint = it }
            s.optString("pbk").takeIf { it.isNotEmpty() }?.let { realityPubKey = it }
            s.optString("sid").takeIf { it.isNotEmpty() }?.let { realityShortId = it }
            val pacEnc = s.optInt("pac_enc", 0)
            if (pacEnc > 0) packetEncoding = pacEnc
            if (s.optInt("mux_s", 0) > 0) enableMux = true
        }
    }
}
