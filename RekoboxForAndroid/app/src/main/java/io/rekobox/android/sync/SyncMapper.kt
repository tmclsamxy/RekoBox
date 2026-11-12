package io.rekobox.android.sync

import io.rekobox.android.database.ProxyEntity
import io.rekobox.android.database.ProxyGroup
import io.rekobox.android.fmt.AbstractBean
import io.rekobox.android.fmt.http.HttpBean
import io.rekobox.android.fmt.hysteria.HysteriaBean
import io.rekobox.android.fmt.naive.NaiveBean
import io.rekobox.android.fmt.shadowsocks.ShadowsocksBean
import io.rekobox.android.fmt.socks.SOCKSBean
import io.rekobox.android.fmt.trojan.TrojanBean
import io.rekobox.android.fmt.tuic.TuicBean
import io.rekobox.android.fmt.v2ray.StandardV2RayBean
import io.rekobox.android.fmt.v2ray.VMessBean
import io.rekobox.android.ktx.applyDefaultValues
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
        return try {
            when (bean) {
                is VMessBean -> {
                    val typeName = if ((bean.alterId ?: 0) == -1) "vless" else "vmess"
                    typeName to v2rayToJson(bean, typeName)
                }

                is TrojanBean -> "trojan" to v2rayToJson(bean, "trojan")
                is HttpBean -> "http" to httpToJson(bean)
                is SOCKSBean -> "socks" to socksToJson(bean)
                is ShadowsocksBean -> "shadowsocks" to ssToJson(bean)
                // hysteria v1 与 Windows 的 hysteria2 不是同一个东西，v1 交给 foreign 透传
                is HysteriaBean ->
                    if ((bean.protocolVersion ?: 2) == 1) null else "hysteria2" to hysteriaToJson(bean)

                is TuicBean -> "tuic" to tuicToJson(bean)
                is NaiveBean -> "naive" to naiveToJson(bean)
                else -> null
            }
        } catch (e: Exception) {
            // 单个条目出错不影响整轮同步：当作不支持，原样透传
            io.rekobox.android.ktx.Logs.w("export profile failed", e)
            null
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

    /** nekoray QUICBean(proxy_Hysteria2)：password=认证串，obfsPassword=混淆密码 */
    private fun hysteriaToJson(bean: HysteriaBean): JSONObject {
        val o = base(bean)
        bean.authPayload?.takeIf { it.isNotEmpty() }?.let { o.put("password", it) }
        bean.obfuscation?.takeIf { it.isNotEmpty() }?.let { o.put("obfsPassword", it) }
        (bean.uploadMbps ?: 0).takeIf { it > 0 }?.let { o.put("uploadMbps", it) }
        (bean.downloadMbps ?: 0).takeIf { it > 0 }?.let { o.put("downloadMbps", it) }
        bean.sni?.takeIf { it.isNotEmpty() }?.let { o.put("sni", it) }
        bean.alpn?.takeIf { it.isNotEmpty() }?.let { o.put("alpn", it) }
        if (bean.allowInsecure == true) o.put("allowInsecure", true)
        bean.caText?.takeIf { it.isNotEmpty() }?.let { o.put("caText", it) }
        (bean.hopInterval ?: 0).takeIf { it > 0 }?.let { o.put("hopInterval", it) }
        // 端口跳跃：只有确实是一段范围时才有意义
        val ports = bean.serverPorts ?: ""
        if (ports.isNotEmpty() && ports != bean.serverPort?.toString()) o.put("hopPort", ports)
        (bean.streamReceiveWindow ?: 0).takeIf { it > 0 }?.let { o.put("streamReceiveWindow", it) }
        (bean.connectionReceiveWindow ?: 0).takeIf { it > 0 }?.let { o.put("connectionReceiveWindow", it) }
        if (bean.disableMtuDiscovery == true) o.put("disableMtuDiscovery", true)
        return o
    }

    /** nekoray QUICBean(proxy_TUIC)：password=token */
    private fun tuicToJson(bean: TuicBean): JSONObject {
        val o = base(bean)
        bean.uuid?.takeIf { it.isNotEmpty() }?.let { o.put("uuid", it) }
        bean.token?.takeIf { it.isNotEmpty() }?.let { o.put("password", it) }
        bean.congestionController?.takeIf { it.isNotEmpty() }?.let { o.put("congestionControl", it) }
        bean.udpRelayMode?.takeIf { it.isNotEmpty() }?.let { o.put("udpRelayMode", it) }
        if (bean.reduceRTT == true) o.put("zeroRttHandshake", true)
        bean.sni?.takeIf { it.isNotEmpty() }?.let { o.put("sni", it) }
        bean.alpn?.takeIf { it.isNotEmpty() }?.let { o.put("alpn", it) }
        if (bean.allowInsecure == true) o.put("allowInsecure", true)
        if (bean.disableSNI == true) o.put("disableSni", true)
        bean.caText?.takeIf { it.isNotEmpty() }?.let { o.put("caText", it) }
        return o
    }

    private fun naiveToJson(bean: NaiveBean): JSONObject {
        val o = base(bean)
        bean.username?.takeIf { it.isNotEmpty() }?.let { o.put("username", it) }
        bean.password?.takeIf { it.isNotEmpty() }?.let { o.put("password", it) }
        bean.proto?.takeIf { it.isNotEmpty() }?.let { o.put("protocol", it) }
        bean.extraHeaders?.takeIf { it.isNotEmpty() }?.let { o.put("extra_headers", it) }
        bean.sni?.takeIf { it.isNotEmpty() }?.let { o.put("sni", it) }
        bean.certificates?.takeIf { it.isNotEmpty() }?.let { o.put("certificate", it) }
        (bean.insecureConcurrency ?: 0).takeIf { it > 0 }?.let { o.put("insecure_concurrency", it) }
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

                "hysteria2" -> HysteriaBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.protocolVersion = 2
                    bean.authPayloadType = HysteriaBean.TYPE_STRING
                    bean.authPayload = beanJson.optString("password")
                    bean.obfuscation = beanJson.optString("obfsPassword")
                    bean.uploadMbps = beanJson.optInt("uploadMbps", 0)
                    bean.downloadMbps = beanJson.optInt("downloadMbps", 0)
                    bean.sni = beanJson.optString("sni")
                    bean.alpn = beanJson.optString("alpn")
                    bean.allowInsecure = beanJson.optBoolean("allowInsecure")
                    bean.caText = beanJson.optString("caText")
                    bean.hopInterval = beanJson.optInt("hopInterval", 10)
                    bean.serverPorts = beanJson.optString("hopPort")
                        .ifEmpty { bean.serverPort?.toString() ?: "443" }
                    bean.streamReceiveWindow = beanJson.optInt("streamReceiveWindow", 0)
                    bean.connectionReceiveWindow = beanJson.optInt("connectionReceiveWindow", 0)
                    bean.disableMtuDiscovery = beanJson.optBoolean("disableMtuDiscovery")
                }

                "tuic" -> TuicBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.protocolVersion = 5
                    bean.uuid = beanJson.optString("uuid")
                    bean.token = beanJson.optString("password")
                    bean.congestionController = beanJson.optString("congestionControl", "bbr")
                    bean.udpRelayMode = beanJson.optString("udpRelayMode", "native")
                    bean.reduceRTT = beanJson.optBoolean("zeroRttHandshake")
                    bean.sni = beanJson.optString("sni")
                    bean.alpn = beanJson.optString("alpn")
                    bean.allowInsecure = beanJson.optBoolean("allowInsecure")
                    bean.disableSNI = beanJson.optBoolean("disableSni")
                    bean.caText = beanJson.optString("caText")
                }

                "naive" -> NaiveBean().also { bean ->
                    applyBase(bean, beanJson)
                    bean.username = beanJson.optString("username")
                    bean.password = beanJson.optString("password")
                    bean.proto = beanJson.optString("protocol", "https")
                    bean.extraHeaders = beanJson.optString("extra_headers")
                    bean.sni = beanJson.optString("sni")
                    bean.certificates = beanJson.optString("certificate")
                    bean.insecureConcurrency = beanJson.optInt("insecure_concurrency", 0)
                }

                else -> null
            }
        } catch (_: Exception) {
            null
        }?.also {
            // 必须补默认值：bean 的 serialize() 会直接 writeInt(protocol/mtu 等)，
            // 这些 Integer 若为 null，落库时会 NPE（"Integer.intValue() on a null object"）
            it.applyDefaultValues()
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
