package io.rekobox.win.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlin.math.cos
import kotlin.math.sin

/**
 * 工具栏图标：NekoRay 用的是 Qt 资源位图，这里用 Canvas 画等价的线性图标，
 * 好处是零资源文件、能跟主题色走。
 */
@Composable
fun ToolIcon(kind: ToolIconKind, tint: Color, size: Dp = 20.dp) {
    Canvas(Modifier.size(size)) {
        val w = this.size.width
        val h = this.size.height
        val sw = w * 0.09f
        when (kind) {
            ToolIconKind.GEAR -> {
                drawCircle(tint, radius = w * 0.26f, style = Stroke(sw))
                drawCircle(tint, radius = w * 0.07f)
                repeat(8) { i ->
                    val a = Math.toRadians(i * 45.0)
                    val r1 = w * 0.30f
                    val r2 = w * 0.42f
                    drawLine(
                        tint,
                        Offset(w / 2 + (cos(a) * r1).toFloat(), h / 2 + (sin(a) * r1).toFloat()),
                        Offset(w / 2 + (cos(a) * r2).toFloat(), h / 2 + (sin(a) * r2).toFloat()),
                        strokeWidth = sw,
                    )
                }
            }
            ToolIconKind.LIST -> {
                listOf(0.28f, 0.5f, 0.72f).forEach { y ->
                    drawLine(tint, Offset(w * 0.16f, h * y), Offset(w * 0.84f, h * y), sw)
                }
            }
            ToolIconKind.SERVER -> {
                drawRoundRect(tint, Offset(w * 0.14f, h * 0.18f), Size(w * 0.72f, h * 0.26f), CornerRadius(2f), style = Stroke(sw))
                drawRoundRect(tint, Offset(w * 0.14f, h * 0.56f), Size(w * 0.72f, h * 0.26f), CornerRadius(2f), style = Stroke(sw))
                drawCircle(tint, w * 0.045f, Offset(w * 0.26f, h * 0.31f))
                drawCircle(tint, w * 0.045f, Offset(w * 0.26f, h * 0.69f))
            }
            ToolIconKind.HORN -> {
                val path = Path().apply {
                    moveTo(w * 0.16f, h * 0.42f)
                    lineTo(w * 0.46f, h * 0.42f)
                    lineTo(w * 0.72f, h * 0.22f)
                    lineTo(w * 0.72f, h * 0.78f)
                    lineTo(w * 0.46f, h * 0.58f)
                    lineTo(w * 0.16f, h * 0.58f)
                    close()
                }
                drawPath(path, tint, style = Stroke(sw))
                drawLine(tint, Offset(w * 0.80f, h * 0.36f), Offset(w * 0.90f, h * 0.28f), sw)
                drawLine(tint, Offset(w * 0.80f, h * 0.64f), Offset(w * 0.90f, h * 0.72f), sw)
            }
            ToolIconKind.DOC -> {
                val path = Path().apply {
                    moveTo(w * 0.22f, h * 0.14f)
                    lineTo(w * 0.60f, h * 0.14f)
                    lineTo(w * 0.78f, h * 0.32f)
                    lineTo(w * 0.78f, h * 0.86f)
                    lineTo(w * 0.22f, h * 0.86f)
                    close()
                }
                drawPath(path, tint, style = Stroke(sw))
                drawLine(tint, Offset(w * 0.60f, h * 0.14f), Offset(w * 0.60f, h * 0.32f), sw)
                drawLine(tint, Offset(w * 0.60f, h * 0.32f), Offset(w * 0.78f, h * 0.32f), sw)
            }
            ToolIconKind.REFRESH -> {
                drawArc(
                    tint, startAngle = 40f, sweepAngle = 280f, useCenter = false,
                    topLeft = Offset(w * 0.18f, h * 0.18f),
                    size = Size(w * 0.64f, h * 0.64f),
                    style = Stroke(sw),
                )
                val path = Path().apply {
                    moveTo(w * 0.62f, h * 0.10f)
                    lineTo(w * 0.86f, h * 0.20f)
                    lineTo(w * 0.66f, h * 0.34f)
                    close()
                }
                drawPath(path, tint)
            }
            ToolIconKind.POWER -> {
                drawArc(
                    tint, startAngle = 120f, sweepAngle = 300f, useCenter = false,
                    topLeft = Offset(w * 0.20f, h * 0.22f),
                    size = Size(w * 0.60f, h * 0.60f),
                    style = Stroke(sw),
                )
                drawLine(tint, Offset(w * 0.5f, h * 0.12f), Offset(w * 0.5f, h * 0.46f), sw)
            }
        }
    }
}

enum class ToolIconKind { GEAR, LIST, SERVER, HORN, DOC, REFRESH, POWER }
