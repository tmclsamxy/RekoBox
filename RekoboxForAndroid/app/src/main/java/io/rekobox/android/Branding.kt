package io.rekobox.android

/**
 * 品牌与外部链接的集中配置。
 *
 * 换仓库 / 换社群链接时只改这一个文件，避免 AboutFragment 里散落硬编码 URL。
 * 注意：API_RELEASES_* 指向的 GitHub 仓库必须真实存在，否则「检查更新」会抛异常。
 */
object Branding {

    const val REPO_OWNER = "tmclsamxy"
    const val REPO_NAME = "RekoBoxForAndroid"

    const val REPO_URL = "https://github.com/$REPO_OWNER/$REPO_NAME"
    const val RELEASES_URL = "$REPO_URL/releases"
    const val API_RELEASES_LATEST =
        "https://api.github.com/repos/$REPO_OWNER/$REPO_NAME/releases/latest"
    const val API_RELEASES_PREVIEW =
        "https://api.github.com/repos/$REPO_OWNER/$REPO_NAME/releases/tags/preview"

    /** 崩溃报告 / User-Agent 里使用的对外名称 */
    const val APP_NAME = "RekoBox"
    const val APP_NAME_LONG = "RekoBox for Android"

    /** 备份文件名前缀、WebDAV 默认目录 */
    const val BACKUP_PREFIX = "rekobox_backup"
    const val WEBDAV_DIR = "RekoBox"
}
