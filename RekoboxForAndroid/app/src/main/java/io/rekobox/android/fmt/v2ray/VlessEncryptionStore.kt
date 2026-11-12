package io.rekobox.android.fmt.v2ray

import io.rekobox.android.SagerNet
import java.io.File

/**
 * VLESS Encryption（mlkem768x25519plus）串的旁路存储。
 *
 * 该串约 1600 字符，实测在 profile 的 Bean 经 Room/DB 往返后会被丢弃
 * （UI 进程读到的 blob 含该字段，VPN 服务进程读到的 blob 不含），
 * 结果是下发给 sing-box 的 VLESS 出站缺少 `encryption`，服务端在
 * ML-KEM 握手阶段直接关闭连接 —— 表现为连接超时、只有上传流量。
 *
 * 因此导入时按 uuid 落盘一份原文，构建配置时若 Bean 字段为空则回填。
 */
object VlessEncryptionStore {

    private fun dir(): File = File(SagerNet.application.filesDir, "vless_enc").apply { mkdirs() }

    private fun fileFor(uuid: String): File = File(dir(), uuid.replace('/', '_'))

    fun put(uuid: String, value: String) {
        if (uuid.isBlank() || value.isBlank()) return
        fileFor(uuid).writeText(value)
    }

    fun get(uuid: String): String? {
        if (uuid.isBlank()) return null
        return runCatching {
            fileFor(uuid).takeIf { it.isFile }?.readText()?.trim()?.takeIf { it.isNotBlank() }
        }.getOrNull()
    }
}
