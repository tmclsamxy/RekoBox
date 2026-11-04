package io.rekobox.core.clash

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration

data class TrafficSample(val upload: Long, val download: Long)

/**
 * sing-box 的 Clash API 客户端。
 *
 * Android 版靠 AIDL 直接问 Go 层要流量/连接；Windows 版改成走 HTTP 控制面
 * （experimental.clash_api），这样 GUI 与核心彻底解耦，换内核版本不用改 UI。
 */
class ClashApi(
    private val controller: String = "127.0.0.1:9090",
    private val secret: String? = null,
) {

    private val base = if (controller.startsWith("http")) controller else "http://$controller"

    private val client: HttpClient = HttpClient.newBuilder()
        .connectTimeout(Duration.ofSeconds(3))
        .build()

    private fun req(path: String, method: String = "GET"): HttpRequest.Builder =
        HttpRequest.newBuilder(URI.create(base + path))
            .timeout(Duration.ofSeconds(5))
            .apply { secret?.takeIf { it.isNotBlank() }?.let { header("Authorization", "Bearer $it") } }
            .method(method, HttpRequest.BodyPublishers.noBody())

    fun isAlive(): Boolean = runCatching {
        client.send(req("/version").build(), HttpResponse.BodyHandlers.ofString()).statusCode() == 200
    }.getOrDefault(false)

    fun version(): String? = runCatching {
        val body = client.send(req("/version").build(), HttpResponse.BodyHandlers.ofString()).body()
        Json.parseToJsonElement(body).jsonObject["version"]?.jsonPrimitive?.content
    }.getOrNull()

    fun proxies(): JsonObject? = runCatching {
        val body = client.send(req("/proxies").build(), HttpResponse.BodyHandlers.ofString()).body()
        Json.parseToJsonElement(body).jsonObject
    }.getOrNull()

    /** 切换 selector / url-test 组的节点 */
    fun select(group: String, name: String): Boolean = runCatching {
        val body = buildJsonObject { put("name", JsonPrimitive(name)) }.toString()
        val r = client.send(
            HttpRequest.newBuilder(URI.create("$base/proxies/$group"))
                .timeout(Duration.ofSeconds(5))
                .header("Content-Type", "application/json")
                .apply { secret?.takeIf { it.isNotBlank() }?.let { header("Authorization", "Bearer $it") } }
                .PUT(HttpRequest.BodyPublishers.ofString(body))
                .build(),
            HttpResponse.BodyHandlers.ofString()
        )
        r.statusCode() in 200..299
    }.getOrDefault(false)

    /** /traffic 是长连接 SSE-free 的逐行 JSON 流，每秒一条 */
    fun traffic(): Flow<TrafficSample> = flow {
        val request = req("/traffic").build()
        val response = client.send(request, HttpResponse.BodyHandlers.ofLines())
        response.body().use { lines ->
            val cursor = lines.iterator()
            while (cursor.hasNext()) {
                val line = cursor.next()
                runCatching {
                    val o = Json.parseToJsonElement(line).jsonObject
                    TrafficSample(
                        upload = o["up"]?.jsonPrimitive?.long ?: 0L,
                        download = o["down"]?.jsonPrimitive?.long ?: 0L,
                    )
                }.getOrNull()?.let { emit(it) }
            }
        }
    }.flowOn(Dispatchers.IO)

    fun logs(level: String = "info"): Flow<String> = flow {
        val request = req("/logs?level=$level").build()
        val response = client.send(request, HttpResponse.BodyHandlers.ofLines())
        response.body().use { lines ->
            val cursor = lines.iterator()
            while (cursor.hasNext()) {
                val line = cursor.next()
                val text = runCatching {
                    val o = Json.parseToJsonElement(line).jsonObject
                    o["payload"]?.jsonPrimitive?.content ?: line
                }.getOrElse { line }
                emit(text)
            }
        }
    }.flowOn(Dispatchers.IO)

    /** 单节点延迟（毫秒）；超时或失败返回 null */
    fun delay(tag: String, timeoutMs: Int = 5000, testUrl: String = "https://www.gstatic.com/generate_204"): Int? =
        runCatching {
            val path = "/proxies/${java.net.URLEncoder.encode(tag, "UTF-8")}/delay?timeout=$timeoutMs&url=${
                java.net.URLEncoder.encode(testUrl, "UTF-8")
            }"
            val body = client.send(req(path).build(), HttpResponse.BodyHandlers.ofString()).body()
            Json.parseToJsonElement(body).jsonObject["delay"]?.jsonPrimitive?.int
        }.getOrNull()?.takeIf { it > 0 }

    /** 读 selector / urltest 组当前选中的节点名 */
    fun groupNow(group: String = "proxy"): String? = runCatching {
        proxies()?.get(group)?.jsonObject?.get("now")?.jsonPrimitive?.content
    }.getOrNull()

    /** 组内可选节点名 */
    fun groupMembers(group: String = "proxy"): List<String> = runCatching {
        proxies()?.get(group)?.jsonObject?.get("all")?.jsonArray
            ?.mapNotNull { (it as? JsonPrimitive)?.content }
            .orEmpty()
    }.getOrDefault(emptyList())

    fun connections(): JsonObject? = runCatching {
        val body = client.send(req("/connections").build(), HttpResponse.BodyHandlers.ofString()).body()
        Json.parseToJsonElement(body).jsonObject
    }.getOrNull()

    fun closeAllConnections() {
        runCatching { client.send(req("/connections", "DELETE").build(), HttpResponse.BodyHandlers.ofString()) }
    }

    /** /connections 的扁平化视图，给「连接」标签页用 */
    fun connectionList(): List<ConnInfo> = runCatching {
        val obj = connections() ?: return emptyList()
        obj["connections"]?.jsonArray?.mapNotNull { el ->
            val o = el.jsonObject
            val meta = o["metadata"]?.jsonObject
            val host = meta?.get("host")?.jsonPrimitive?.content
                ?: meta?.get("destinationIP")?.jsonPrimitive?.content
                ?: ""
            val port = meta?.get("destinationPort")?.jsonPrimitive?.content.orEmpty()
            val chains = o["chains"]?.jsonArray?.joinToString(" → ") { it.jsonPrimitive.content }.orEmpty()
            ConnInfo(
                host = if (port.isNotEmpty()) "$host:$port" else host,
                chains = chains,
                upload = o["upload"]?.jsonPrimitive?.long ?: 0L,
                download = o["download"]?.jsonPrimitive?.long ?: 0L,
            )
        }.orEmpty()
    }.getOrDefault(emptyList())
}

data class ConnInfo(
    val host: String,
    val chains: String,
    val upload: Long,
    val download: Long,
)
