package io.rekobox.android.update

import android.os.Build
import io.rekobox.android.Branding
import io.rekobox.android.BuildConfig
import io.rekobox.android.database.DataStore
import io.rekobox.android.nb4a.utils.Util
import libcore.Libcore
import org.json.JSONArray
import org.json.JSONObject

/**
 * 版本检测。
 *
 * 发布信息取自 GitHub Releases 列表接口，而不是 /releases/latest：
 * 仓库同时承载 Android（tag `vX.Y.Z`）与 Windows（tag `win-vX.Y.Z`）两条发布线，
 * /latest 只返回全仓库最新的那个，会拿到另一个平台的包。这里按 tag 前缀过滤。
 */
object UpdateChecker {

    data class UpdateInfo(
        val versionName: String,
        val tagName: String,
        val changelog: String,
        val pageUrl: String,
        val downloadUrl: String,
        val assetName: String,
        val size: Long,
        val preview: Boolean,
    )

    private fun releasesApi(): String =
        "https://api.github.com/repos/${Branding.REPO_OWNER}/${Branding.REPO_NAME}/releases?per_page=30"

    /**
     * @return 有更新返回 [UpdateInfo]，已是最新或没有可用资产返回 null
     */
    fun check(includePreview: Boolean = false): UpdateInfo? {
        val json = fetchReleases()
        val releases = JSONArray(json)
        var best: JSONObject? = null
        var bestVersion = ""

        for (i in 0 until releases.length()) {
            val release = releases.optJSONObject(i) ?: continue
            if (release.optBoolean("draft")) continue
            val preview = release.optBoolean("prerelease")
            if (preview && !includePreview) continue

            val tag = release.optString("tag_name")
            val version = androidVersionOf(tag) ?: continue
            if (best == null || compareVersion(version, bestVersion) > 0) {
                best = release
                bestVersion = version
            }
        }

        val release = best ?: return null
        if (compareVersion(bestVersion, BuildConfig.VERSION_NAME) <= 0) return null

        val asset = pickAsset(release.optJSONArray("assets") ?: JSONArray()) ?: return null
        return UpdateInfo(
            versionName = bestVersion,
            tagName = release.optString("tag_name"),
            changelog = release.optString("body").trim(),
            pageUrl = release.optString("html_url"),
            downloadUrl = asset.optString("browser_download_url"),
            assetName = asset.optString("name"),
            size = asset.optLong("size"),
            preview = release.optBoolean("prerelease"),
        )
    }

    /**
     * 优先走内核 mixed 入站（用户当前选中的节点），失败再直连。
     * 未连接时端口不监听，第一次必然失败，回退一次即可。
     */
    private fun fetchReleases(): String {
        var lastError: Exception? = null
        for (viaProxy in listOf(true, false)) {
            try {
                return fetchOnce(viaProxy)
            } catch (e: Exception) {
                lastError = e
            }
        }
        throw lastError ?: IllegalStateException("check failed")
    }

    private fun fetchOnce(viaProxy: Boolean): String {
        val client = Libcore.newHttpClient().apply {
            modernTLS()
            if (viaProxy && DataStore.mixedPort > 0) {
                trySocks5(
                    DataStore.mixedPort,
                    DataStore.mixedInboundUser,
                    DataStore.mixedInboundPass
                )
            }
        }
        val response = client.newRequest().apply {
            setURL(releasesApi())
            setHeader("Accept", "application/vnd.github+json")
            setUserAgent("${Branding.APP_NAME}/${BuildConfig.VERSION_NAME}")
        }.execute()
        return Util.getStringBox(response.contentString)
    }

    /** tag 形如 v1.0.1；win-v1.0.4 / pre-1.0.2-1 都会被排除或单独处理 */
    internal fun androidVersionOf(tag: String): String? {
        if (tag.startsWith("v")) {
            val v = tag.substring(1)
            return if (v.isNotEmpty() && v[0].isDigit()) v else null
        }
        if (tag.startsWith("pre-")) {
            // pre-1.0.2-1 -> 1.0.2
            val core = tag.removePrefix("pre-").substringBeforeLast("-")
            return if (core.isNotEmpty() && core[0].isDigit()) core else null
        }
        return null
    }

    /**
     * 语义化比较：忽略前缀，按 . 分段转整数逐段比，缺位补 0。
     * 返回 >0 表示 a 比 b 新。
     */
    fun compareVersion(a: String, b: String): Int {
        val pa = a.trim().split(".")
        val pb = b.trim().split(".")
        for (i in 0 until maxOf(pa.size, pb.size)) {
            val x = pa.getOrNull(i)?.toIntOrNull() ?: 0
            val y = pb.getOrNull(i)?.toIntOrNull() ?: 0
            if (x != y) return x - y
        }
        return 0
    }

    /** 按设备 ABI 优先级选 APK，找不到精确匹配时退回第一个 apk */
    private fun pickAsset(assets: JSONArray): JSONObject? {
        val apks = (0 until assets.length())
            .mapNotNull { assets.optJSONObject(it) }
            .filter { it.optString("name").endsWith(".apk", ignoreCase = true) }
        if (apks.isEmpty()) return null

        for (abi in Build.SUPPORTED_ABIS) {
            apks.firstOrNull { it.optString("name").contains("-$abi.apk", ignoreCase = true) }
                ?.let { return it }
        }
        return apks.first()
    }
}
