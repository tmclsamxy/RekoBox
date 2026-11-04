package io.rekobox.core

import io.rekobox.core.config.ConfigBuilder
import io.rekobox.core.model.EngineSettings
import io.rekobox.core.model.Profile
import io.rekobox.core.model.RunMode
import io.rekobox.core.profile.ProfileParser
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.util.Base64
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

class ProfileParserTest {

    private val b64 = { s: String -> Base64.getEncoder().encodeToString(s.toByteArray()) }

    @Test
    fun `ss SIP002 带 base64 userinfo`() {
        val p = ProfileParser.parse("ss://${b64("aes-256-gcm:pass123")}@1.2.3.4:8388#%E9%A6%99%E6%B8%AF")
        assertEquals("shadowsocks", p.type)
        assertEquals("1.2.3.4", p.server)
        assertEquals(8388, p.port)
        assertEquals("香港", p.name)
        assertEquals("aes-256-gcm", p.outbound["method"]?.jsonPrimitive?.content)
        assertEquals("pass123", p.outbound["password"]?.jsonPrimitive?.content)
    }

    @Test
    fun `ss 带 obfs 插件`() {
        val p = ProfileParser.parse("ss://${b64("aes-128-gcm:pw")}@a.com:443?plugin=obfs-local%3Bobfs%3Dhttp%3Bobfs-host%3Dbing.com#obfs")
        assertEquals("obfs-local", p.outbound["plugin"]?.jsonPrimitive?.content)
        assertEquals("obfs=http;obfs-host=bing.com", p.outbound["plugin_opts"]?.jsonPrimitive?.content)
    }

    @Test
    fun `vmess base64 json`() {
        val inner = """{"v":"2","ps":"东京","add":"jp.example.com","port":"443","id":"11111111-2222-3333-4444-555555555555","aid":"0","scy":"auto","net":"ws","host":"cdn.example.com","path":"/ray","tls":"tls","sni":"cdn.example.com"}"""
        val p = ProfileParser.parse("vmess://${b64(inner)}")
        assertEquals("vmess", p.type)
        assertEquals("东京", p.name)
        assertEquals(443, p.port)
        val tls = p.outbound["tls"]?.jsonObject
        assertEquals(true, tls?.get("enabled")?.jsonPrimitive?.content?.toBoolean())
        val tr = p.outbound["transport"]?.jsonObject
        assertEquals("ws", tr?.get("type")?.jsonPrimitive?.content)
        assertEquals("/ray", tr?.get("path")?.jsonPrimitive?.content)
    }

    @Test
    fun `vless reality ws`() {
        val link = "vless://uuid-1@1.1.1.1:443?encryption=none&security=reality&sni=www.apple.com&fp=chrome&pbk=PUBKEY&sid=ab&type=ws&path=%2Fws&host=cdn.apple.com#reality"
        val p = ProfileParser.parse(link)
        assertEquals("vless", p.type)
        val tls = p.outbound["tls"]!!.jsonObject
        assertEquals("www.apple.com", tls["server_name"]!!.jsonPrimitive.content)
        assertEquals("PUBKEY", tls["reality"]!!.jsonObject["public_key"]!!.jsonPrimitive.content)
        assertEquals("chrome", tls["utls"]!!.jsonObject["fingerprint"]!!.jsonPrimitive.content)
    }

    @Test
    fun `trojan grpc`() {
        val p = ProfileParser.parse("trojan://secret@t.example.com:443?security=tls&sni=t.example.com&type=grpc&serviceName=svc#trojan")
        assertEquals("trojan", p.type)
        assertEquals("secret", p.outbound["password"]!!.jsonPrimitive.content)
        assertEquals("svc", p.outbound["transport"]!!.jsonObject["service_name"]!!.jsonPrimitive.content)
    }

    @Test
    fun `hysteria2 带 obfs`() {
        val p = ProfileParser.parse("hysteria2://pw@hy.example.com:8443?sni=hy.example.com&insecure=1&obfs=salamander&obfs-password=xyz#hy2")
        assertEquals("hysteria2", p.type)
        assertEquals(true, p.outbound["tls"]!!.jsonObject["insecure"]!!.jsonPrimitive.content.toBoolean())
        assertEquals("salamander", p.outbound["obfs"]!!.jsonObject["type"]!!.jsonPrimitive.content)
        assertEquals("xyz", p.outbound["obfs"]!!.jsonObject["password"]!!.jsonPrimitive.content)
    }

    @Test
    fun `tuic 默认 h3`() {
        val p = ProfileParser.parse("tuic://uuid:pw@tuic.example.com:443?sni=tuic.example.com#tuic")
        assertEquals("tuic", p.type)
        assertEquals("uuid", p.outbound["uuid"]!!.jsonPrimitive.content)
        assertEquals("h3", p.outbound["tls"]!!.jsonObject["alpn"]!!.jsonArray[0].jsonPrimitive.content)
    }

    @Test
    fun `订阅整体 base64 多行`() {
        val body = b64(
            "ss://${b64("aes-256-gcm:p1")}@1.1.1.1:1111#n1\n" +
                "trojan://p2@2.2.2.2:443?sni=a.com#n2\n" +
                "vless://u3@3.3.3.3:443?security=tls&type=tcp#n3"
        )
        val result = ProfileParser.parseSubscription(body)
        assertEquals(3, result.profiles.size)
        assertTrue(result.errors.isEmpty())
    }

    @Test
    fun `IPv6 地址`() {
        val p = ProfileParser.parse("ss://${b64("aes-256-gcm:p")}@[2001:db8::1]:8388#v6")
        assertEquals("2001:db8::1", p.server)
        assertEquals(8388, p.port)
    }
}

class ConfigBuilderTest {

    private val nodes = listOf(
        Profile(name = "A", type = "shadowsocks", server = "1.1.1.1", port = 1, outbound = Json.parseToJsonElement("""{"type":"shadowsocks","server":"1.1.1.1","server_port":1,"method":"aes-128-gcm","password":"p"}""").jsonObject),
        Profile(name = "B", type = "trojan", server = "2.2.2.2", port = 2, outbound = Json.parseToJsonElement("""{"type":"trojan","server":"2.2.2.2","server_port":2,"password":"p"}""").jsonObject),
    )

    private fun cfg(settings: EngineSettings = EngineSettings(), selected: String? = null) =
        Json.parseToJsonElement(ConfigBuilder.build(nodes, selected, settings)).jsonObject

    @Test
    fun `selector 组包含全部节点且默认选中`() {
        val outbounds = cfg(selected = nodes[1].tag)["outbounds"]!!.jsonArray
        val selector = outbounds.first { it.jsonObject["tag"]?.jsonPrimitive?.content == "proxy" }.jsonObject
        assertEquals("selector", selector["type"]!!.jsonPrimitive.content)
        assertEquals(nodes[1].tag, selector["default"]!!.jsonPrimitive.content)
        val members = selector["outbounds"]!!.jsonArray.map { it.jsonPrimitive.content }
        assertTrue(members.containsAll(listOf(nodes[0].tag, nodes[1].tag, "direct")))
    }

    @Test
    fun `节点 tag 唯一并写进 outbound`() {
        val outbounds = cfg()["outbounds"]!!.jsonArray.map { it.jsonObject }
        val tags = outbounds.mapNotNull { it["tag"]?.jsonPrimitive?.content }
        assertEquals(tags.size, tags.toSet().size)
        assertTrue(tags.containsAll(listOf(nodes[0].tag, nodes[1].tag)))
    }

    @Test
    fun `TUN 模式才有 tun 入站`() {
        val tun = cfg(EngineSettings(runMode = RunMode.TUN))["inbounds"]!!.jsonArray
        assertTrue(tun.any { it.jsonObject["type"]?.jsonPrimitive?.content == "tun" })

        val proxy = cfg(EngineSettings(runMode = RunMode.SYSTEM_PROXY))["inbounds"]!!.jsonArray
        assertTrue(proxy.none { it.jsonObject["type"]?.jsonPrimitive?.content == "tun" })
        assertTrue(proxy.any { it.jsonObject["type"]?.jsonPrimitive?.content == "mixed" })
    }

    @Test
    fun `自动选择时生成 urltest 组`() {
        val outbounds = cfg(EngineSettings(autoSelect = true))["outbounds"]!!.jsonArray.map { it.jsonObject }
        val auto = outbounds.firstOrNull { it["tag"]?.jsonPrimitive?.content == "auto" }
        assertNotNull(auto)
        assertEquals("urltest", auto["type"]!!.jsonPrimitive.content)
    }

    @Test
    fun `没有节点时明确报错`() {
        val error = runCatching { ConfigBuilder.build(emptyList(), null, EngineSettings()) }.exceptionOrNull()
        assertNotNull(error)
        assertTrue(error!!.message!!.contains("没有可用节点"))
    }

    /**
     * 真内核校验：找到 runtime/windows/sing-box.exe 就把生成的配置喂给 `sing-box check`。
     * 这是唯一能证明字段名没写错的办法（schema 随版本变化，单测断言挡不住）。
     * 找不到内核时静默跳过，不阻塞 CI。
     */
    @Test
    fun `生成的配置能被 sing-box check 接受`() {
        val core = System.getenv("REKOBOX_CORE")?.let { java.io.File(it) }
            ?: java.io.File(System.getProperty("user.dir")).resolve("../runtime/windows/sing-box.exe").normalize()
        if (!core.exists()) return

        listOf(RunMode.TUN, RunMode.SYSTEM_PROXY).forEach { mode ->
            val cfg = ConfigBuilder.build(
                nodes,
                nodes[0].tag,
                EngineSettings(runMode = mode, mixedPort = 12080, clashPort = 19090),
            )
            val tmp = java.io.File("build/sample-config-${mode.name.lowercase()}.json")
            tmp.parentFile.mkdirs()
            tmp.writeText(cfg)
            val p = ProcessBuilder(core.absolutePath, "check", "-c", tmp.absolutePath)
                .redirectErrorStream(true)
                .start()
            val out = p.inputStream.bufferedReader().readText()
            val code = p.waitFor()
            assertEquals(0, code, "sing-box check 失败（$mode）：$out")
        }
    }
}
