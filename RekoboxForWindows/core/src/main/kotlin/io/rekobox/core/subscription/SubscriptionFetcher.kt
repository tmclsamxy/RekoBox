package io.rekobox.core.subscription

import io.rekobox.core.model.Profile
import io.rekobox.core.profile.ProfileParser
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.nio.file.Files
import java.nio.file.Paths
import java.time.Duration

/**
 * 订阅拉取。
 *
 * Android 版的订阅走 `SubscriptionFragment` + OkHttp + 定时 Worker；Windows 版
 * 只需要「拉一次、解析、入库」，定时更新交给界面层的协程。
 */
class SubscriptionFetcher(
    private val userAgent: String = "Rekobox/1.0 (Windows)",
) {

    data class Result(val profiles: List<Profile>, val errors: List<String>)

    private val client: HttpClient = HttpClient.newBuilder()
        .connectTimeout(Duration.ofSeconds(15))
        .followRedirects(HttpClient.Redirect.NORMAL)
        .build()

    fun fetch(url: String): Result {
        val body = if (url.startsWith("file://")) {
            Files.readString(Paths.get(URI.create(url)))
        } else {
            val request = HttpRequest.newBuilder(URI.create(url))
                .timeout(Duration.ofSeconds(30))
                .header("User-Agent", userAgent)
                .header("Accept", "*/*")
                .GET()
                .build()
            val response = client.send(request, HttpResponse.BodyHandlers.ofString())
            if (response.statusCode() !in 200..299) {
                throw IllegalStateException("HTTP ${response.statusCode()}")
            }
            response.body()
        }
        return ProfileParser.parseSubscription(body).let { Result(it.profiles, it.errors) }
    }
}
