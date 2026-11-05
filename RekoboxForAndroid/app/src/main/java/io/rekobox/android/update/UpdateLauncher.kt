package io.rekobox.android.update

import android.app.Activity
import android.content.Context
import io.rekobox.android.ktx.Logs
import io.rekobox.android.ktx.runOnIoDispatcher
import io.rekobox.android.ktx.runOnMainDispatcher

/**
 * 启动时自动检查更新。
 *
 * 用 SharedPreferences 做 24 小时节流：每次冷启动都打一次 GitHub API 会撞匿名限流
 * （60 次/小时/IP），而且对用户没有意义。
 */
object UpdateLauncher {

    private const val PREF = "update_check"
    private const val KEY_LAST_CHECK = "last_check_at"
    private const val MIN_INTERVAL_MS = 24L * 60 * 60 * 1000

    fun checkOnStart(activity: Activity) {
        val sp = activity.getSharedPreferences(PREF, Context.MODE_PRIVATE)
        val now = System.currentTimeMillis()
        if (now - sp.getLong(KEY_LAST_CHECK, 0) < MIN_INTERVAL_MS) return
        sp.edit().putLong(KEY_LAST_CHECK, now).apply()

        val appContext = activity.applicationContext
        runOnIoDispatcher {
            val info = try {
                UpdateChecker.check()
            } catch (e: Exception) {
                // 启动检查是静默的，失败不打扰用户；手动检查才报错
                Logs.w(e)
                null
            } ?: return@runOnIoDispatcher

            runOnMainDispatcher {
                if (!activity.isFinishing) UpdatePrompt.show(activity, info)
            }
        }
    }
}
