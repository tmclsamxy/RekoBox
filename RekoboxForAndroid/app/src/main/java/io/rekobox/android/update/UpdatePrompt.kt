package io.rekobox.android.update

import android.app.Activity
import android.app.AlertDialog
import android.widget.Toast
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import io.rekobox.android.BuildConfig
import io.rekobox.android.R
import io.rekobox.android.ktx.Logs
import io.rekobox.android.ktx.readableMessage
import io.rekobox.android.ktx.runOnIoDispatcher
import io.rekobox.android.ktx.runOnMainDispatcher

/**
 * 更新提示 → 下载 → 拉起安装器的完整交互。
 */
object UpdatePrompt {

    fun show(activity: Activity, info: UpdateChecker.UpdateInfo) {
        val size = if (info.size > 0) String.format("%.1f MB", info.size / 1048576.0) else "?"
        var message = activity.getString(
            R.string.update_dialog_message,
            BuildConfig.VERSION_NAME,
            "${info.versionName} ($size)"
        )
        if (info.changelog.isNotBlank()) {
            message += "\n\n" + activity.getString(R.string.update_changelog_title) + "\n" +
                    info.changelog.take(800)
        }

        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.update_dialog_title)
            .setMessage(message)
            .setPositiveButton(R.string.yes) { _, _ -> startDownload(activity, info) }
            .setNegativeButton(R.string.no, null)
            .show()
    }

    private fun startDownload(activity: Activity, info: UpdateChecker.UpdateInfo) {
        if (!ApkInstaller.canInstallPackages(activity)) {
            MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.update_dialog_title)
                .setMessage(R.string.update_install_permission_message)
                .setPositiveButton(R.string.update_install_permission_go) { _, _ ->
                    ApkInstaller.openInstallPermissionSettings(activity)
                }
                .setNegativeButton(R.string.no, null)
                .show()
            return
        }

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.update_downloading)
            .setMessage("")
            .setCancelable(false)
            .create()
        dialog.show()

        runOnIoDispatcher {
            try {
                val file = UpdateDownloader.download(activity.applicationContext, info) { done, total ->
                    runOnMainDispatcher {
                        if (activity.isFinishing) return@runOnMainDispatcher
                        val text = if (total > 0) {
                            "${readableSize(done)} / ${readableSize(total)}  ${done * 100 / total}%"
                        } else {
                            readableSize(done)
                        }
                        dialog.setMessage(text)
                    }
                }
                runOnMainDispatcher {
                    dialog.dismiss()
                    if (!activity.isFinishing) ApkInstaller.install(activity, file)
                }
            } catch (e: Exception) {
                Logs.w(e)
                runOnMainDispatcher {
                    dialog.dismiss()
                    if (!activity.isFinishing) {
                        Toast.makeText(
                            activity,
                            activity.getString(R.string.update_download_failed, e.readableMessage),
                            Toast.LENGTH_LONG
                        ).show()
                    }
                }
            }
        }
    }

    private fun readableSize(bytes: Long): String = when {
        bytes >= 1048576 -> String.format("%.1f MB", bytes / 1048576.0)
        bytes >= 1024 -> String.format("%.0f KB", bytes / 1024.0)
        else -> "$bytes B"
    }
}
