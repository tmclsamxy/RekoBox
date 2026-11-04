package io.rekobox.win

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.graphics.painter.Painter
import androidx.compose.ui.graphics.toComposeImageBitmap
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Tray
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import io.rekobox.win.platform.SingleInstance
import io.rekobox.win.ui.App
import io.rekobox.win.ui.RekoboxTheme
import java.awt.BasicStroke
import java.awt.Font
import java.awt.RenderingHints
import java.awt.image.BufferedImage
import javax.swing.JOptionPane

fun main(args: Array<String>) {
    if (!SingleInstance.acquire()) {
        JOptionPane.showMessageDialog(
            null,
            "Rekobox 已经在运行，请从系统托盘打开主界面。",
            "Rekobox",
            JOptionPane.INFORMATION_MESSAGE,
        )
        return
    }

    val startMinimized = args.contains("--minimized")

    application {
        val scope = rememberCoroutineScope()
        val controller = remember { AppController(scope) }
        var windowVisible by remember { mutableStateOf(!startMinimized) }
        val icon = remember { trayIcon() }

        fun quit() {
            controller.shutdown()
            SingleInstance.release()
            exitApplication()
        }

        Tray(
            icon = icon,
            tooltip = "Rekobox",
            onAction = { windowVisible = true },
            menu = {
                Item("显示主界面") { windowVisible = true }
                Separator()
                Item(if (controller.running) "断开连接" else "连接") {
                    if (controller.running) controller.stop()
                    else controller.start(onRelaunch = { quit() })
                }
                Separator()
                Item("退出 Rekobox") { quit() }
            },
        )

        Window(
            onCloseRequest = {
                if (controller.settings.closeToTray) windowVisible = false else quit()
            },
            visible = windowVisible,
            title = "Rekobox",
            state = rememberWindowState(size = DpSize(1120.dp, 760.dp)),
            icon = icon,
        ) {
            RekoboxTheme {
                App(controller, onRelaunch = { quit() })
            }
        }
    }
}

/**
 * 托盘与窗口图标：不依赖资源文件，运行时用 AWT 画一个青色圆环 + R。
 * 打包成 MSI 时可以用同样思路生成 .ico，省掉图标资源管理。
 */private fun trayIcon(): Painter {
    val size = 64
    val image = BufferedImage(size, size, BufferedImage.TYPE_INT_ARGB)
    val g = image.createGraphics()
    g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON)
    g.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL, RenderingHints.VALUE_STROKE_PURE)

    g.color = java.awt.Color(0x0B, 0x12, 0x20)
    g.fillRoundRect(0, 0, size, size, 16, 16)

    g.color = java.awt.Color(0x22, 0xD3, 0xEE)
    g.stroke = BasicStroke(4.5f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND)
    g.drawArc(9, 9, 46, 46, 40, 280)

    g.font = Font("Segoe UI", Font.BOLD, 30)
    g.drawString("R", 21, 42)
    g.dispose()

    return BitmapPainter(image.toComposeImageBitmap())
}
