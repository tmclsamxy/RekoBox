package io.rekobox.android.nb4a.ui

import android.content.Context
import android.widget.TextView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import io.rekobox.android.R
import io.rekobox.android.ktx.Logs
import io.rekobox.android.ktx.readableMessage
import io.rekobox.android.ktx.runOnMainDispatcher

object Dialogs {
    fun logExceptionAndShow(context: Context, e: Exception, callback: Runnable) {
        Logs.e(e)
        runOnMainDispatcher {
            MaterialAlertDialogBuilder(context)
                .setTitle(R.string.error_title)
                .setMessage(e.readableMessage)
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    callback.run()
                }
                .show()
        }
    }

    fun message(context: Context, title: String, message: String) {
        runOnMainDispatcher {
            val dialog = MaterialAlertDialogBuilder(context)
                .setTitle(title)
                .setMessage(message)
                .setCancelable(true)
                .show()
            dialog.findViewById<TextView>(android.R.id.message)?.apply {
                setTextIsSelectable(true)
            }
        }
    }
}