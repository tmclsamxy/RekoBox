package io.rekobox.android.sync

import org.json.JSONObject
import java.io.BufferedInputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * 极薄的 HTTP 包装：Bearer 头、20s 超时、统一错误文案。
 * 用 HttpURLConnection 而不是引入 OkHttp —— 只发几个小请求，没必要加依赖。
 */
object SyncApi {

    private const val TIMEOUT_MS = 20_000

    class Result(val status: Int, val body: String?) {
        val ok: Boolean get() = status in 200..299
        val networkError: Boolean get() = status == 0

        private val json: JSONObject? by lazy {
            try {
                body?.takeIf { it.isNotBlank() }?.let { JSONObject(it) }
            } catch (_: Exception) {
                null
            }
        }

        val code: String get() = json?.optJSONObject("error")?.optString("code") ?: ""
        val error: String
            get() = when {
                status == 0 -> body ?: "网络错误"
                ok -> ""
                else -> json?.optJSONObject("error")?.optString("message")?.takeIf { it.isNotBlank() }
                    ?: "服务端返回 HTTP $status"
            }
    }

    fun get(server: String, path: String, token: String): Result =
        request("GET", server, path, token, null)

    fun post(server: String, path: String, token: String, payload: JSONObject? = null): Result =
        request("POST", server, path, token, payload)

    fun put(server: String, path: String, token: String, payload: JSONObject): Result =
        request("PUT", server, path, token, payload)

    private fun request(
        method: String,
        server: String,
        path: String,
        token: String,
        payload: JSONObject?,
    ): Result {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(server + path).openConnection() as HttpURLConnection).apply {
                requestMethod = method
                connectTimeout = TIMEOUT_MS
                readTimeout = TIMEOUT_MS
                setRequestProperty("Accept", "application/json")
                setRequestProperty("User-Agent", "RekoBox-Android-Sync/1")
                if (token.isNotEmpty()) setRequestProperty("Authorization", "Bearer $token")
                if (payload != null) {
                    doOutput = true
                    setRequestProperty("Content-Type", "application/json")
                }
            }
            if (payload != null) {
                conn.outputStream.use { it.write(payload.toString().toByteArray()) }
            }
            val status = conn.responseCode
            val stream = if (status in 200..299) conn.inputStream else conn.errorStream
            val body = stream?.let { BufferedInputStream(it).readBytes().toString(Charsets.UTF_8) }
            Result(status, body)
        } catch (e: Exception) {
            Result(0, e.message ?: "连接失败")
        } finally {
            conn?.disconnect()
        }
    }
}
