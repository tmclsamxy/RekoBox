package io.rekobox.win.platform

import io.rekobox.core.AppPaths
import java.nio.channels.FileChannel
import java.nio.channels.FileLock
import java.nio.file.StandardOpenOption

/**
 * 单实例锁。
 *
 * 托盘常驻的程序最怕多开：两个进程抢同一个端口、各写一份系统代理，退出时还会互相踩。
 * 用 %LOCALAPPDATA%\Rekobox\.instance.lock 的排他锁做闸门。
 */
object SingleInstance {

    private var channel: FileChannel? = null
    private var lock: FileLock? = null

    fun acquire(): Boolean = runCatching {
        val file = AppPaths.dataDir.resolve(".instance.lock")
        channel = FileChannel.open(
            file,
            StandardOpenOption.CREATE,
            StandardOpenOption.WRITE,
        )
        lock = channel!!.tryLock()
        lock != null
    }.getOrDefault(false)

    fun release() {
        runCatching { lock?.release() }
        runCatching { channel?.close() }
        lock = null
        channel = null
    }
}
