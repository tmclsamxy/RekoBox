package io.rekobox.android.sync

import android.util.Base64
import io.rekobox.android.fmt.AbstractBean
import io.rekobox.android.fmt.http.parseHttp
import io.rekobox.android.fmt.hysteria.parseHysteria2
import io.rekobox.android.fmt.naive.parseNaive
import io.rekobox.android.fmt.shadowsocks.parseShadowsocks
import io.rekobox.android.fmt.socks.parseSOCKS
import io.rekobox.android.fmt.trojan.parseTrojan
import io.rekobox.android.fmt.tuic.parseTuic
import io.rekobox.android.fmt.v2ray.parseV2Ray
import org.json.JSONObject

/**
 * 把服务端下发的分享链接还原成节点配置。
 *
 * 服务端只存链接本身（并解析出类型/地址/端口用于展示），具体协议参数由客户端解析，
 * 所以服务端加新协议不用改代码。这里放行的协议与服务端 `server/nodes.go` 的清单一致。
 */
object LinkParser {

    fun parse(link: String): AbstractBean? {
        val s = link.trim()
        if (s.isEmpty()) return null
        val bean = runCatching {
            when {
                s.startsWith("vmess://") -> parseVmess(s)
                s.startsWith("vless://") -> parseV2Ray(s)
                s.startsWith("trojan://") -> parseTrojan(s)
                s.startsWith("ss://") -> parseShadowsocks(s)
                s.startsWith("socks5://") || s.startsWith("socks4://") ||
                    s.startsWith("socks4a://") || s.startsWith("socks://") -> parseSOCKS(s)

                s.startsWith("http://") || s.startsWith("https://") -> parseHttp(s)
                s.startsWith("naive+") -> parseNaive(s)
                s.startsWith("hysteria2://") || s.startsWith("hy2://") -> parseHysteria2(s)
                s.startsWith("tuic://") -> parseTuic(s)
                else -> null
            }
        }.getOrNull() ?: return null

        // 部分字段是 Java 的 Boolean/Integer 可空类型，解析器只填有值的那些；
        // 不补默认值，落库时 Kryo 序列化会拆箱空指针
        runCatching { bean.initializeDefaultValues() }
        return bean
    }

    /**
     * vmess 链接宽容解析：内置解析器要求 JSON 里带 net/type 等字段，
     * 而部分面板只给 add/port/id/ps（Windows 端能连）。这里补齐缺省字段再解析一次。
     */
    private fun parseVmess(link: String): AbstractBean {
        runCatching { parseV2Ray(link) }.getOrNull()?.let { return it }

        val body = link.substringAfter("vmess://", "").substringBefore("#")
        val json = runCatching {
            JSONObject(String(decodeB64Lenient(body), Charsets.UTF_8))
        }.getOrNull() ?: throw IllegalArgumentException("vmess 链接解析失败")

        if (json.optString("net").isEmpty()) json.put("net", "tcp")
        if (!json.has("type")) json.put("type", "none")
        if (!json.has("aid")) json.put("aid", "0")
        if (!json.has("scy")) json.put("scy", "auto")
        if (!json.has("tls")) json.put("tls", "")

        val encoded = Base64.encodeToString(
            json.toString().toByteArray(Charsets.UTF_8),
            Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP
        )
        return parseV2Ray("vmess://$encoded")
    }

    private fun decodeB64Lenient(body: String): ByteArray {
        val flags = Base64.NO_WRAP
        return runCatching { Base64.decode(body, Base64.URL_SAFE or flags) }
            .recoverCatching { Base64.decode(body, Base64.DEFAULT) }
            .getOrThrow()
    }
}
