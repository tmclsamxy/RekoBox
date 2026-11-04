package io.rekobox.core.profile

import io.rekobox.core.model.Profile
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonArray
import kotlinx.serialization.json.putJsonObject
import java.net.URLDecoder
import java.util.Base64

/**
 * 分享链接解析。
 *
 * Android 版靠 `fmt/` 里每个协议一个类 + gson 模型；Windows 版直接在 core 里产出
 * sing-box 原生 outbound JSON —— 少一层中间模型，新增协议只改这一个文件。
 *
 * 已支持：ss / vmess / vless / trojan / hysteria2 / tuic / anytls / socks5 / http
 * 不支持：ssr（sing-box 1.10 已移除 shadowsocksr outbound）、wireguard（字段太多，建议直接粘 outbound JSON）
 */
object ProfileParser {

    private val json = Json { ignoreUnknownKeys = true; isLenient = true }

    data class ParseResult(val profiles: List<Profile>, val errors: List<String>)

    /** 单条链接；失败抛 IllegalArgumentException，消息可直接展示给用户 */
    fun parse(link: String): Profile = parseAll(link).let {
        it.profiles.firstOrNull() ?: throw IllegalArgumentException(it.errors.firstOrNull() ?: "无法解析")
    }

    /** 订阅正文 → 多条节点。整体 base64、明文多行、CRLF 都吃 */
    fun parseSubscription(body: String): ParseResult = parseAll(body, decodeWhole = true)

    fun parseAll(text: String, decodeWhole: Boolean = false): ParseResult {
        var body = text.trim()
        if (decodeWhole && !body.contains("://")) body = runCatching { b64(body) }.getOrDefault(body)

        val profiles = mutableListOf<Profile>()
        val errors = mutableListOf<String>()
        body.lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() && !it.startsWith("#") && !it.startsWith("//") }
            .forEach { line ->
                runCatching { parseLink(line) }
                    .onSuccess { profiles += it }
                    .onFailure { errors += "${line.take(24)}… → ${it.message}" }
            }
        if (profiles.isEmpty() && errors.isEmpty()) errors += "内容为空"
        return ParseResult(profiles, errors)
    }

    // ---------------------------------------------------------------- 分发

    private fun parseLink(raw: String): Profile {
        val link = raw.trim()
        val scheme = link.substringBefore("://", "").lowercase()
        if (scheme.isEmpty()) {
            if (link.startsWith("{")) return fromOutboundJson(link)
            throw IllegalArgumentException("不是分享链接")
        }
        return when (scheme) {
            "ss" -> parseShadowsocks(link)
            "ssr" -> throw IllegalArgumentException("SSR 已从 sing-box 1.10 移除，请改用 SS/VMess")
            "vmess" -> parseVmess(link)
            "vless" -> parseVless(link)
            "trojan" -> parseTrojan(link)
            "hysteria2", "hy2" -> parseHysteria2(link)
            "tuic" -> parseTuic(link)
            "anytls" -> parseAnyTls(link)
            "socks", "socks5" -> parseSocksLike(link, "socks")
            "http", "https" -> parseSocksLike(link, "http")
            else -> throw IllegalArgumentException("暂不支持 $scheme://")
        }
    }

    private fun fromOutboundJson(text: String): Profile {
        val obj = json.parseToJsonElement(text).jsonObject
        val type = obj.str("type") ?: "custom"
        val server = obj.str("server") ?: ""
        val port = obj.str("server_port")?.toIntOrNull() ?: 0
        return Profile(
            name = "$type ${if (server.isNotEmpty()) "$server:$port" else "自定义"}",
            type = type,
            server = server,
            port = port,
            outbound = obj,
            rawLink = text,
        )
    }

    // ---------------------------------------------------------------- Shadowsocks

    /** SIP002：ss://base64(method:pass)@host:port#tag；也兼容整体 base64 的老格式 */
    private fun parseShadowsocks(link: String): Profile {
        val (body, name) = splitName(link.removePrefix("ss://"), "Shadowsocks")
        val noQuery = body.substringBefore('?')
        val query = parseQuery(body.substringAfter('?', ""))

        val (userInfo, hostPort) = if (noQuery.contains("@")) {
            noQuery.substringBeforeLast('@') to noQuery.substringAfterLast('@')
        } else {
            val decoded = b64(noQuery)
            decoded.substringBeforeLast('@') to decoded.substringAfterLast('@')
        }
        val cred = if (userInfo.contains(":")) userInfo else b64(userInfo)
        val method = cred.substringBefore(':')
        val password = cred.substringAfter(':', "")
        val (host, port) = splitHostPort(hostPort)

        return Profile(
            name = name,
            type = "shadowsocks",
            server = host,
            port = port,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", "shadowsocks")
                put("server", host)
                put("server_port", port)
                put("method", method)
                put("password", password)
                query["plugin"]?.takeIf { it.isNotBlank() }?.let { plugin ->
                    val (pname, popts) = parsePlugin(plugin)
                    put("plugin", pname)
                    popts?.let { put("plugin_opts", it) }
                }
            },
        )
    }

    /** plugin=obfs-local;obfs=http;obfs-host=a.com → ("obfs-local", "obfs=http;obfs-host=a.com") */
    private fun parsePlugin(raw: String): Pair<String, String?> {
        val value = runCatching { URLDecoder.decode(raw, "UTF-8") }.getOrDefault(raw)
        val parts = value.split(';')
        val name = when (parts.first()) {
            "obfs-local", "simple-obfs" -> "obfs-local"
            "v2ray-plugin" -> "v2ray-plugin"
            else -> parts.first()
        }
        return name to parts.drop(1).joinToString(";").takeIf { it.isNotBlank() }
    }

    // ---------------------------------------------------------------- VMess

    private fun parseVmess(link: String): Profile {
        val payload = link.removePrefix("vmess://").substringBefore('#')
        val obj = runCatching { json.parseToJsonElement(b64(payload)).jsonObject }
            .getOrElse { throw IllegalArgumentException("vmess 内容不是合法 base64 JSON") }

        fun str(vararg keys: String): String = keys.firstNotNullOfOrNull { obj.str(it) } ?: ""

        val host = str("add")
        val port = str("port").toIntOrNull() ?: 443
        val uuid = str("id")
        if (host.isEmpty() || uuid.isEmpty()) throw IllegalArgumentException("vmess 缺少 add/id")
        val name = str("ps").ifBlank { "VMess $host" }
        val net = str("net").ifBlank { "tcp" }
        val tls = str("tls").equals("tls", true)
        val sni = str("sni", "host")

        return Profile(
            name = name,
            type = "vmess",
            server = host,
            port = port,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", "vmess")
                put("server", host)
                put("server_port", port)
                put("uuid", uuid)
                put("security", str("scy").ifBlank { "auto" })
                put("alter_id", str("aid").toIntOrNull() ?: 0)
                if (tls) {
                    putJsonObject("tls") {
                        put("enabled", true)
                        put("server_name", sni.ifBlank { host })
                        val alpn = str("alpn").split(',').filter { it.isNotBlank() }
                        if (alpn.isNotEmpty()) putJsonArray("alpn") { alpn.forEach { add(it.trim()) } }
                    }
                }
                transport(net, str("host"), str("path"), str("type"), str("serviceName"))
            },
        )
    }

    // ---------------------------------------------------------------- VLESS

    private fun parseVless(link: String): Profile {
        val (body, name) = splitName(link.removePrefix("vless://"), "VLESS")
        val parts = splitUri(body)
        if (parts.userInfo.isBlank() || parts.host.isBlank()) throw IllegalArgumentException("vless 缺少 uuid/地址")

        val security = parts.query["security"].orEmpty()
        val net = parts.query["type"].orEmpty().ifBlank { "tcp" }

        return Profile(
            name = name,
            type = "vless",
            server = parts.host,
            port = parts.port,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", "vless")
                put("server", parts.host)
                put("server_port", parts.port)
                put("uuid", parts.userInfo)
                parts.query["flow"]?.takeIf { it.isNotBlank() }?.let { put("flow", it) }
                tls(parts.query, security, parts.host)
                transport(net, parts.query["host"], parts.query["path"], parts.query["headerType"], parts.query["serviceName"])
            },
        )
    }

    // ---------------------------------------------------------------- Trojan

    private fun parseTrojan(link: String): Profile {
        val (body, name) = splitName(link.removePrefix("trojan://"), "Trojan")
        val parts = splitUri(body)
        if (parts.userInfo.isBlank() || parts.host.isBlank()) throw IllegalArgumentException("trojan 缺少密码/地址")

        return Profile(
            name = name,
            type = "trojan",
            server = parts.host,
            port = parts.port,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", "trojan")
                put("server", parts.host)
                put("server_port", parts.port)
                put("password", parts.userInfo)
                tls(parts.query, parts.query["security"].orEmpty().ifBlank { "tls" }, parts.host)
                transport(
                    parts.query["type"].orEmpty().ifBlank { "tcp" },
                    parts.query["host"], parts.query["path"], parts.query["headerType"], parts.query["serviceName"],
                )
            },
        )
    }

    // ---------------------------------------------------------------- Hysteria2

    private fun parseHysteria2(link: String): Profile {
        val (body, name) = splitName(link.substringAfter("://"), "Hysteria2")
        val parts = splitUri(body)
        val password = parts.userInfo.substringAfter(':', "").ifBlank { parts.userInfo }
        if (parts.host.isBlank()) throw IllegalArgumentException("hysteria2 缺少地址")

        return Profile(
            name = name,
            type = "hysteria2",
            server = parts.host,
            port = parts.port,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", "hysteria2")
                put("server", parts.host)
                put("server_port", parts.port)
                put("password", password)
                putJsonObject("tls") {
                    put("enabled", true)
                    put("server_name", parts.query["sni"] ?: parts.query["peer"] ?: parts.host)
                    if (parts.query.isTrue("insecure") || parts.query.isTrue("allowInsecure")) put("insecure", true)
                    val alpn = parts.query["alpn"]?.split(',')?.filter { it.isNotBlank() }
                    if (!alpn.isNullOrEmpty()) putJsonArray("alpn") { alpn.forEach { add(it.trim()) } }
                }
                parts.query["obfs"]?.takeIf { it.isNotBlank() }?.let { obfs ->
                    putJsonObject("obfs") {
                        put("type", obfs)
                        put("password", parts.query["obfs-password"].orEmpty())
                    }
                }
                parts.query["up"]?.let { put("up_mbps", it.filter { c -> c.isDigit() }.toIntOrNull() ?: 0) }
                parts.query["down"]?.let { put("down_mbps", it.filter { c -> c.isDigit() }.toIntOrNull() ?: 0) }
            },
        )
    }

    // ---------------------------------------------------------------- TUIC

    private fun parseTuic(link: String): Profile {
        val (body, name) = splitName(link.removePrefix("tuic://"), "TUIC")
        val parts = splitUri(body)
        val uuid = parts.userInfo.substringBefore(':')
        val password = parts.userInfo.substringAfter(':', "")

        return Profile(
            name = name,
            type = "tuic",
            server = parts.host,
            port = parts.port,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", "tuic")
                put("server", parts.host)
                put("server_port", parts.port)
                put("uuid", uuid)
                put("password", password)
                put("congestion_control", parts.query["congestion_control"] ?: "bbr")
                put("udp_relay_mode", parts.query["udp_relay_mode"] ?: "native")
                putJsonObject("tls") {
                    put("enabled", true)
                    put("server_name", parts.query["sni"] ?: parts.host)
                    if (parts.query.isTrue("allow_insecure") || parts.query.isTrue("insecure")) put("insecure", true)
                    putJsonArray("alpn") {
                        (parts.query["alpn"]?.split(',')?.filter { it.isNotBlank() } ?: listOf("h3"))
                            .forEach { add(it.trim()) }
                    }
                }
            },
        )
    }

    // ---------------------------------------------------------------- AnyTLS

    private fun parseAnyTls(link: String): Profile {
        val (body, name) = splitName(link.removePrefix("anytls://"), "AnyTLS")
        val parts = splitUri(body)

        return Profile(
            name = name,
            type = "anytls",
            server = parts.host,
            port = parts.port,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", "anytls")
                put("server", parts.host)
                put("server_port", parts.port)
                put("password", parts.userInfo)
                tls(parts.query, "tls", parts.host)
            },
        )
    }

    // ---------------------------------------------------------------- SOCKS / HTTP

    private fun parseSocksLike(link: String, type: String): Profile {
        val scheme = link.substringBefore("://")
        val (body, name) = splitName(link.substringAfter("://"), if (type == "http") "HTTP" else "SOCKS")
        val parts = splitUri(body)
        val defaultPort = if (type == "http") 8080 else 1080

        return Profile(
            name = name,
            type = type,
            server = parts.host,
            port = if (parts.port > 0) parts.port else defaultPort,
            rawLink = link,
            outbound = buildJsonObject {
                put("type", type)
                put("server", parts.host.ifBlank { "127.0.0.1" })
                put("server_port", if (parts.port > 0) parts.port else defaultPort)
                if (parts.userInfo.contains(':')) {
                    put("username", parts.userInfo.substringBefore(':'))
                    put("password", parts.userInfo.substringAfter(':', ""))
                }
                if (scheme == "https") putJsonObject("tls") { put("enabled", true) }
            },
        )
    }

    // ---------------------------------------------------------------- 公共构件

    private fun JsonObjectBuilder.tls(query: Map<String, String>, security: String, host: String) {
        val enabled = security.equals("tls", true) || security.equals("reality", true) || security.equals("xtls", true)
        if (!enabled) return
        putJsonObject("tls") {
            put("enabled", true)
            put("server_name", query["sni"] ?: query["peer"] ?: host)
            if (query.isTrue("allowInsecure") || query.isTrue("insecure") || query.isTrue("skip-cert-verify")) {
                put("insecure", true)
            }
            val alpn = query["alpn"]?.split(',')?.filter { it.isNotBlank() }
            if (!alpn.isNullOrEmpty()) putJsonArray("alpn") { alpn.forEach { add(it.trim()) } }
            query["fp"]?.takeIf { it.isNotBlank() }?.let { fp ->
                putJsonObject("utls") {
                    put("enabled", true)
                    put("fingerprint", fp)
                }
            }
            if (security.equals("reality", true) && !query["pbk"].isNullOrBlank()) {
                putJsonObject("reality") {
                    put("enabled", true)
                    put("public_key", query["pbk"])
                    put("short_id", query["sid"].orEmpty())
                }
            }
        }
    }

    private fun JsonObjectBuilder.transport(
        net: String,
        host: String?,
        path: String?,
        headerType: String?,
        serviceName: String?,
    ) {
        when (net.lowercase()) {
            "ws" -> putJsonObject("transport") {
                put("type", "ws")
                put("path", path?.takeIf { it.isNotBlank() } ?: "/")
                host?.takeIf { it.isNotBlank() }?.let { h ->
                    putJsonObject("headers") { put("Host", h) }
                }
            }
            "grpc" -> putJsonObject("transport") {
                put("type", "grpc")
                put("service_name", serviceName?.takeIf { it.isNotBlank() } ?: path.orEmpty())
            }
            "http", "h2" -> putJsonObject("transport") {
                put("type", "http")
                putJsonArray("host") { add(host?.takeIf { it.isNotBlank() } ?: "") }
                put("path", path?.takeIf { it.isNotBlank() } ?: "/")
            }
            "httpupgrade" -> putJsonObject("transport") {
                put("type", "httpupgrade")
                host?.takeIf { it.isNotBlank() }?.let { put("host", it) }
                put("path", path?.takeIf { it.isNotBlank() } ?: "/")
            }
            "tcp" -> if (headerType.equals("http", true)) {
                putJsonObject("transport") {
                    put("type", "http")
                    putJsonArray("host") { add(host?.takeIf { it.isNotBlank() } ?: "") }
                    put("path", path?.takeIf { it.isNotBlank() } ?: "/")
                }
            }
            else -> Unit
        }
    }

    // ---------------------------------------------------------------- 字符串工具

    private fun splitName(bodyWithFragment: String, fallback: String): Pair<String, String> {
        val body = bodyWithFragment.substringBefore('#')
        val frag = bodyWithFragment.substringAfter('#', "")
        val name = if (frag.isBlank()) fallback else runCatching { URLDecoder.decode(frag, "UTF-8") }.getOrDefault(frag)
        return body to name
    }

    /** 拆 userinfo@host:port?query —— 不用 URI，避免密码里的特殊字符把解析炸掉 */
    private fun splitUri(body: String): UriParts {
        val queryPart = body.substringAfter('?', "")
        val main = body.substringBefore('?')
        val userInfo = if (main.contains('@')) main.substringBeforeLast('@') else ""
        val hostPort = main.substringAfterLast('@')
        val (host, port) = splitHostPort(hostPort)
        return UriParts(
            userInfo = runCatching { URLDecoder.decode(userInfo, "UTF-8") }.getOrDefault(userInfo),
            host = host,
            port = port,
            query = parseQuery(queryPart),
        )
    }

    private data class UriParts(val userInfo: String, val host: String, val port: Int, val query: Map<String, String>)

    /** 支持 [::1]:443 与 host:443，端口缺省回落到 443 */
    private fun splitHostPort(raw: String): Pair<String, Int> {
        val s = raw.trim().removeSuffix("/")
        if (s.startsWith("[")) {
            val host = s.substringAfter('[').substringBefore(']')
            val port = s.substringAfter("]:", "").toIntOrNull() ?: 443
            return host to port
        }
        val idx = s.lastIndexOf(':')
        if (idx < 0) return s to 443
        val maybePort = s.substring(idx + 1).toIntOrNull() ?: return s to 443
        return s.substring(0, idx) to maybePort
    }

    private fun parseQuery(raw: String): Map<String, String> {
        if (raw.isBlank()) return emptyMap()
        return raw.split('&').mapNotNull { kv ->
            if (kv.isBlank()) return@mapNotNull null
            val k = kv.substringBefore('=')
            val v = kv.substringAfter('=', "")
            URLDecoder.decode(k, "UTF-8") to URLDecoder.decode(v, "UTF-8")
        }.toMap()
    }

    private fun Map<String, String>.isTrue(key: String): Boolean =
        this[key]?.let { it == "1" || it.equals("true", true) } == true

    private fun JsonObject.str(key: String): String? = (this[key] as? JsonPrimitive)?.content

    private fun b64(s: String): String {
        val cleaned = s.trim().replace("\n", "").replace("\r", "").replace(' ', '+')
        val t = cleaned.replace('-', '+').replace('_', '/')
        val pad = t.length % 4
        val padded = if (pad == 0) t else t + "=".repeat(4 - pad)
        return String(Base64.getDecoder().decode(padded), Charsets.UTF_8)
    }
}
