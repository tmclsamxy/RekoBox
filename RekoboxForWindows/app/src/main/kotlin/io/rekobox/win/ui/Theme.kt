package io.rekobox.win.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ---------------------------------------------------------------- 配色（对齐 NekoRay / Windows 原生浅色）
val Accent = Color(0xFF2A7FFF)
val Bg = Color(0xFFFFFFFF)
val ToolbarBg = Color(0xFFF3F3F3)
val PanelBg = Color(0xFFFAFAFA)
val Border = Color(0xFFDCDCDC)
val BorderLight = Color(0xFFE8E8E8)
val TextPrimary = Color(0xFF1F1F1F)
val TextMuted = Color(0xFF5A5A5A)
val TextFaint = Color(0xFF8A8A8A)
val SelectedBg = Color(0xFFCCE8FF)
val HoverBg = Color(0xFFEAF4FD)
val Danger = Color(0xFFC42B1C)
val Success = Color(0xFF107C10)
val Warn = Color(0xFF9D5D00)

// 兼容旧名（对话框里用到）
val Surface1 = PanelBg
val Surface2 = BorderLight
val Surface3 = Border

@Composable
fun RekoboxTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = lightColorScheme(
            primary = Accent,
            onPrimary = Color.White,
            background = Bg,
            onBackground = TextPrimary,
            surface = Bg,
            onSurface = TextPrimary,
            outline = Border,
        ),
        content = content,
    )
}

/** 小号状态标签（订阅异常、节点数） */
@Composable
fun Badge(text: String, color: Color = Accent, filled: Boolean = false) {
    Box(
        Modifier
            .background(if (filled) color.copy(alpha = 0.12f) else Color.Transparent, RoundedCornerShape(3.dp))
            .padding(horizontal = if (filled) 6.dp else 0.dp, vertical = if (filled) 2.dp else 0.dp)
    ) {
        Text(text, color = color, fontSize = 11.sp, fontWeight = FontWeight.Medium)
    }
}

/** 对话框里的分组卡片 */
@Composable
fun SectionCard(
    title: String,
    subtitle: String? = null,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Column(
        modifier.fillMaxWidth()
            .background(Bg, RoundedCornerShape(4.dp))
            .padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(title, color = TextPrimary, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
            if (subtitle != null) {
                Spacer(Modifier.width(8.dp))
                Text(subtitle, color = TextFaint, fontSize = 11.sp)
            }
        }
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) { content() }
    }
}

@Composable
fun KeyValueRow(label: String, value: String, valueColor: Color = TextMuted) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(88.dp), color = TextFaint, fontSize = 11.5.sp)
        Text(value, color = valueColor, fontSize = 11.5.sp)
    }
}

@Composable
fun Dot(color: Color, size: Int = 8) {
    Box(Modifier.width(size.dp).height(size.dp).background(color, RoundedCornerShape(size.dp)))
}

/** 分组标签（表格上方那一排，形制同 NekoRay 的分组选项卡） */
@Composable
fun TabStripItem(label: String, selected: Boolean, onClick: () -> Unit) {
    Column(
        Modifier
            .background(if (selected) Bg else ToolbarBg)
            .padding(horizontal = 14.dp, vertical = 6.dp),
    ) {
        Text(
            label,
            color = if (selected) TextPrimary else TextMuted,
            fontSize = 12.sp,
            fontWeight = if (selected) FontWeight.Medium else FontWeight.Normal,
        )
    }
}
