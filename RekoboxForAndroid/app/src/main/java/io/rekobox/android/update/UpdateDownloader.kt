package io.rekobox.android.update

import android.content.Context
import io.rekobox.android.database.DataStore
import okhttp3.Credentials
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.net.InetSocketAddress
import java.net.Proxy
import java.util.concurrent.TimeUnit

/**
 * APK 下载。
 *
 * 用 OkHttp 而不是 libcore 的 HTTPClient：后者只有 GetContent() 一次性取回全部字节，
 * 30 MB 的包读进内存且拿不到进度。这里用流式读写并按字节回调。
 *
 * 网络路径与应用其它请求一致：优先走内核的 mixed 入站（即用户当前选中的节点），
 * 端口未监听（未连接）时回退直连。
 */
object UpdateDownloader {

    /**
     * @param onProgress (已下载字节, 总字节)；总字节未知时为 -1
     * @return 下载好的 apk 文件
     */
    fun download(
        context: Context,
        info: UpdateChecker.UpdateInfo,
        onProgress: (Long, Long) -> Unit,
    ): File {
        val dir = File(context.cacheDir, "update").apply { mkdirs() }
        // 清掉上次残留，避免不同版本互相覆盖
        dir.listFiles()?.forEach { it.delete() }
        val target = File(dir, info.assetName)

        var lastError: Exception? = null
        for (viaProxy in listOf(true, false)) {
            try {
                fetch(info, target, viaProxy, onProgress)
                return target
            } catch (e: Exception) {
                lastError = e
                target.delete()
            }
        }
        throw lastError ?: IllegalStateException("download failed")
    }

    private fun fetch(
        info: UpdateChecker.UpdateInfo,
        target: File,
        viaProxy: Boolean,
        onProgress: (Long, Long) -> Unit,
    ) {
        val client = newClient(viaProxy)
        val request = Request.Builder()
            .url(info.downloadUrl)
            .header("User-Agent", "RekoBox-Android")
            .build()

        client.newCall(request).execute().use { response ->
            if (!response.isSuccessful) error("HTTP ${response.code}")
            val body = response.body ?: error("empty body")
            val total = if (body.contentLength() > 0) body.contentLength() else info.size
            var downloaded = 0L
            body.byteStream().use { input ->
                target.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val read = input.read(buffer)
                        if (read == -1) break
                        output.write(buffer, 0, read)
                        downloaded += read
                        onProgress(downloaded, total)
                    }
                    output.flush()
                }
            }
            if (total > 0 && downloaded != total) error("incomplete: $downloaded/$total")
        }
    }

    private fun newClient(viaProxy: Boolean): OkHttpClient {
        val builder = OkHttpClient.Builder()
            .connectTimeout(20, TimeUnit.SECONDS)
            .readTimeout(30, TimeUnit.SECONDS)

        val port = DataStore.mixedPort
        if (viaProxy && port > 0 && port <= 65535) {
            builder.proxy(Proxy(Proxy.Type.SOCKS, InetSocketAddress("127.0.0.1", port)))
            if (DataStore.mixedInboundAuthed) {
                val user = DataStore.mixedInboundUser
                val pass = DataStore.mixedInboundPass
                builder.proxyAuthenticator { _, response ->
                    response.request.newBuilder()
                        .header("Proxy-Authorization", Credentials.basic(user, pass))
                        .build()
                }
            }
        }
        return builder.build()
    }
}
