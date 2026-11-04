package io.rekobox.win.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.rekobox.core.model.Profile
import io.rekobox.core.model.RunMode
import io.rekobox.win.AppController
import io.rekobox.win.Sheet
import io.rekobox.win.label

/**
 * 主窗口：按 NekoRay 的形制复刻。
 *
 *   顶部大图标工具栏（程序 / 首选项 / 服务器 / 推广 / 文档 / 更新 + Tun 模式 / 系统代理）
 *   分组选项卡（默认 / 各订阅）
 *   节点表（类型 | 地址 | 名称 | 测试结果 | 流量）
 *   底部 Dock（日志 / 连接）
 *   状态栏（运行状态 · 端口 · 代理/直连计数）
 */
@Composable
fun App(c: AppController, onRelaunch: () -> Unit) {
    var query by remember { mutableStateOf("") }
    var dockTab by remember { mutableStateOf(0) }

    Column(Modifier.fillMaxSize().background(Bg)) {
        Toolbar(c, onRelaunch, query) { query = it }
        HorizontalDivider(color = Border)

        GroupTabs(c)
        HorizontalDivider(color = Border)

        c.message?.let { MessageBar(c, it) }

        NodeTable(c, query, Modifier.weight(1f))

        HorizontalDivider(color = Border)
        BottomDock(c, dockTab) { dockTab = it }
        HorizontalDivider(color = Border)
        StatusBar(c)
    }

    when (c.sheet) {
        Sheet.IMPORT -> ImportDialog(c)
        Sheet.SUBSCRIPTIONS -> SubscriptionsDialog(c)
        Sheet.SETTINGS -> SettingsDialog(c)
        Sheet.ABOUT -> AboutDialog(c)
        Sheet.NONE -> Unit
    }
}

// ---------------------------------------------------------------- 工具栏

@Composable
private fun Toolbar(c: AppController, onRelaunch: () -> Unit, query: String, onQuery: (String) -> Unit) {
    Row(
        Modifier.fillMaxWidth().background(ToolbarBg).padding(horizontal = 4.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ProgramMenu(c, onRelaunch)
        PreferenceMenu(c)
        ServerMenu(c)
        ToolItem(ToolIconKind.HORN, "推广") { c.sheet = Sheet.ABOUT }
        ToolItem(ToolIconKind.DOC, "文档") {
            runCatching {
                java.awt.Desktop.getDesktop()
                    .browse(java.net.URI("https://github.com/MatsuriDayo/nekoray"))
            }
        }
        ToolItem(ToolIconKind.REFRESH, "更新") {
            c.message = "当前版本 1.0.0；内核 ${c.corePath?.fileName ?: "未安装"}"
        }

        Spacer(Modifier.weight(1f))

        OutlinedTextField(
            value = query,
            onValueChange = onQuery,
            placeholder = { Text("搜索", fontSize = 11.sp) },
            singleLine = true,
            modifier = Modifier.width(150.dp).height(32.dp),
        )
        Spacer(Modifier.width(6.dp))

        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(
                checked = c.settings.runMode == RunMode.TUN,
                onCheckedChange = { checked ->
                    c.updateSettings(
                        c.settings.copy(runMode = if (checked) RunMode.TUN else RunMode.MANUAL)
                    )
                },
            )
            Text("Tun 模式", color = TextPrimary, fontSize = 12.sp)
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(
                checked = c.settings.runMode == RunMode.SYSTEM_PROXY,
                onCheckedChange = { checked ->
                    c.updateSettings(
                        c.settings.copy(
                            runMode = if (checked) RunMode.SYSTEM_PROXY
                            else if (c.settings.runMode == RunMode.SYSTEM_PROXY) RunMode.MANUAL else c.settings.runMode
                        )
                    )
                },
            )
            Text("系统代理", color = TextPrimary, fontSize = 12.sp)
        }
        Spacer(Modifier.width(4.dp))
    }
}

@Composable
private fun ToolItem(kind: ToolIconKind, label: String, hasMenu: Boolean = false, onClick: () -> Unit) {
    Box(
        Modifier
            .width(62.dp)
            .height(52.dp)
            .background(Color.Transparent, RoundedCornerShape(3.dp))
            .clickable { onClick() }
            .padding(top = 6.dp),
        contentAlignment = Alignment.TopCenter,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            ToolIcon(kind, TextPrimary)
            Spacer(Modifier.height(3.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(label, color = TextPrimary, fontSize = 10.5.sp)
                if (hasMenu) Text(" ▾", color = TextFaint, fontSize = 8.sp)
            }
        }
    }
}

@Composable
private fun ProgramMenu(c: AppController, onRelaunch: () -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        ToolItem(ToolIconKind.POWER, "程序", hasMenu = true) { open = true }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(
                text = { Text(if (c.running) "断开" else "启动", fontSize = 12.sp) },
                onClick = {
                    open = false
                    if (c.running) c.stop() else c.start(onRelaunch)
                },
            )
            DropdownMenuItem(
                text = { Text("测试全部延迟", fontSize = 12.sp) },
                enabled = c.running,
                onClick = { open = false; c.testAllDelays() },
            )
            DropdownMenuItem(
                text = { Text("更新全部订阅", fontSize = 12.sp) },
                enabled = c.subscriptions.isNotEmpty(),
                onClick = { open = false; c.refreshAllSubscriptions() },
            )
            HorizontalDivider(color = BorderLight)
            DropdownMenuItem(
                text = { Text((if (c.settings.autostart) "✓ " else "　") + "跟随系统启动", fontSize = 12.sp) },
                onClick = { open = false; c.updateSettings(c.settings.copy(autostart = !c.settings.autostart)) },
            )
        }
    }
}

@Composable
private fun PreferenceMenu(c: AppController) {
    var open by remember { mutableStateOf(false) }
    Box {
        ToolItem(ToolIconKind.GEAR, "首选项", hasMenu = true) { open = true }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(
                text = { Text("分组", fontSize = 12.sp) },
                onClick = { open = false; c.sheet = Sheet.SUBSCRIPTIONS },
            )
            DropdownMenuItem(
                text = { Text("基本设置", fontSize = 12.sp) },
                onClick = { open = false; c.sheet = Sheet.SETTINGS },
            )
            HorizontalDivider(color = BorderLight)
            DropdownMenuItem(
                text = { Text("路由：默认（绕过局域网）", fontSize = 12.sp) },
                onClick = { open = false; c.updateSettings(c.settings.copy(bypassLan = true)) },
            )
            DropdownMenuItem(
                text = { Text("路由：全局", fontSize = 12.sp) },
                onClick = { open = false; c.updateSettings(c.settings.copy(bypassLan = false)) },
            )
        }
    }
}

@Composable
private fun ServerMenu(c: AppController) {
    var open by remember { mutableStateOf(false) }
    Box {
        ToolItem(ToolIconKind.SERVER, "服务器", hasMenu = true) { open = true }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(
                text = { Text("从剪贴板添加", fontSize = 12.sp) },
                onClick = { open = false; c.importFromClipboard() },
            )
            DropdownMenuItem(
                text = { Text("手动输入配置", fontSize = 12.sp) },
                onClick = { open = false; c.sheet = Sheet.IMPORT },
            )
            DropdownMenuItem(
                text = { Text("更新订阅", fontSize = 12.sp) },
                onClick = { open = false; c.sheet = Sheet.SUBSCRIPTIONS },
            )
        }
    }
}

@Composable
private fun MessageBar(c: AppController, text: String) {
    Row(
        Modifier.fillMaxWidth().background(Accent.copy(alpha = 0.10f)).padding(horizontal = 12.dp, vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(text, color = Accent, fontSize = 11.5.sp, modifier = Modifier.weight(1f))
        Text("✕", color = TextFaint, fontSize = 11.sp, modifier = Modifier.clickable { c.message = null })
    }
}

// ---------------------------------------------------------------- 分组选项卡

@Composable
private fun GroupTabs(c: AppController) {
    Row(
        Modifier.fillMaxWidth().background(ToolbarBg).horizontalScroll(rememberScrollState()),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TabStripItem("默认", c.filterSource == null) { c.filterSource = null }
        c.subscriptions.forEach { sub ->
            TabStripItem(sub.name, c.filterSource == sub.id) { c.filterSource = sub.id }
        }
        Box(Modifier.clickable { c.sheet = Sheet.SUBSCRIPTIONS }.padding(horizontal = 10.dp, vertical = 4.dp)) {
            Text("＋", color = TextMuted, fontSize = 13.sp)
        }
    }
}

// ---------------------------------------------------------------- 节点表

@Composable
private fun NodeTable(c: AppController, query: String, modifier: Modifier = Modifier) {
    val rows = c.profiles
        .filter { p ->
            (c.filterSource == null ||
                (c.filterSource == "ungrouped" && p.sourceId == null) ||
                p.sourceId == c.filterSource) &&
                (query.isBlank() || p.name.contains(query, true) || p.server.contains(query, true))
        }
        .let { list -> if (c.delays.isEmpty()) list else list.sortedBy { c.delays[it.id] ?: Int.MAX_VALUE } }

    Column(modifier.fillMaxWidth().background(Bg)) {
        Row(
            Modifier.fillMaxWidth().background(ToolbarBg).padding(horizontal = 8.dp, vertical = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("类型", Modifier.width(112.dp), color = TextMuted, fontSize = 11.5.sp)
            Text("地址", Modifier.width(230.dp), color = TextMuted, fontSize = 11.5.sp)
            Text("名称", Modifier.weight(1f), color = TextMuted, fontSize = 11.5.sp)
            Text("测试结果", Modifier.width(96.dp), color = TextMuted, fontSize = 11.5.sp)
            Text("流量", Modifier.width(70.dp), color = TextMuted, fontSize = 11.5.sp)
            Spacer(Modifier.width(26.dp))
        }
        HorizontalDivider(color = Border)

        LazyColumn(Modifier.weight(1f)) {
            items(rows, key = { it.id }) { p ->
                NodeRow(c, p, selected = p.id == c.saved.selectedId, delay = c.delays[p.id])
            }
            if (rows.isEmpty()) {
                item {
                    Text(
                        if (c.profiles.isEmpty()) "还没有节点。菜单「服务器」→ 从剪贴板添加 / 手动输入配置。"
                        else "该分组下没有节点",
                        color = TextFaint, fontSize = 12.sp, modifier = Modifier.padding(16.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun NodeRow(c: AppController, p: Profile, selected: Boolean, delay: Int?) {
    var menu by remember { mutableStateOf(false) }

    Row(
        Modifier
            .fillMaxWidth()
            .background(if (selected) SelectedBg else Color.Transparent)
            .clickable { c.select(p) }
            .padding(horizontal = 8.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(p.type, Modifier.width(112.dp), color = TextMuted, fontSize = 11.5.sp, maxLines = 1)
        Text(
            p.endpoint, Modifier.width(230.dp), color = TextMuted, fontSize = 11.5.sp,
            fontFamily = FontFamily.Monospace, maxLines = 1, overflow = TextOverflow.Ellipsis,
        )
        Text(
            p.name, Modifier.weight(1f),
            color = if (p.enabled) TextPrimary else TextFaint,
            fontSize = 11.5.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
        )
        Text(
            when {
                delay == null -> ""
                delay > 0 -> "$delay ms"
                else -> "超时"
            },
            Modifier.width(96.dp),
            color = when {
                delay == null -> TextFaint
                delay > 0 && delay < 300 -> Success
                delay > 0 && delay < 800 -> Warn
                else -> Danger
            },
            fontSize = 11.5.sp,
        )
        Text("—", Modifier.width(70.dp), color = TextFaint, fontSize = 11.5.sp)
        Box {
            Text(
                "⋯", color = TextFaint, fontSize = 13.sp,
                modifier = Modifier.clickable { menu = true }.padding(horizontal = 6.dp),
            )
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                DropdownMenuItem(
                    text = { Text("当前分组 → 使用此节点", fontSize = 12.sp) },
                    onClick = { menu = false; c.select(p) },
                )
                DropdownMenuItem(
                    text = { Text("启动", fontSize = 12.sp) },
                    onClick = { menu = false; c.select(p); if (!c.running) c.start() },
                )
                DropdownMenuItem(
                    text = { Text("测试延迟", fontSize = 12.sp) },
                    enabled = c.running,
                    onClick = { menu = false; c.testDelay(p) },
                )
                DropdownMenuItem(
                    text = { Text("复制链接", fontSize = 12.sp) },
                    enabled = p.rawLink.isNotBlank(),
                    onClick = { menu = false; c.copyLink(p) },
                )
                DropdownMenuItem(
                    text = { Text(if (p.enabled) "停用" else "启用", fontSize = 12.sp) },
                    onClick = { menu = false; c.setEnabled(p.id, !p.enabled) },
                )
                DropdownMenuItem(
                    text = { Text("删除", color = Danger, fontSize = 12.sp) },
                    onClick = { menu = false; c.removeProfile(p.id) },
                )
            }
        }
    }
}

// ---------------------------------------------------------------- 底部 Dock

@Composable
private fun BottomDock(c: AppController, tab: Int, onTab: (Int) -> Unit) {
    Column(Modifier.fillMaxWidth().height(186.dp).background(PanelBg)) {
        Row(
            Modifier.fillMaxWidth().background(ToolbarBg).padding(horizontal = 2.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TabStripItem("日志", tab == 0) { onTab(0) }
            TabStripItem("连接${if (c.connections.isNotEmpty()) " (${c.connections.size})" else ""}", tab == 1) { onTab(1) }
            Spacer(Modifier.weight(1f))
            SmallAction("清空") { c.logs.clear() }
            SmallAction("导出") {
                runCatching {
                    val f = io.rekobox.core.AppPaths.logDir.resolve("session-${System.currentTimeMillis()}.log")
                    java.nio.file.Files.writeString(f, c.logs.joinToString("\n"))
                    c.addLog("[界面] 日志已导出到 $f")
                }
            }
            SmallAction(if (c.logsVisible) "隐藏" else "显示") { c.logsVisible = !c.logsVisible }
        }
        HorizontalDivider(color = Border)
        if (c.logsVisible) {
            if (tab == 0) LogView(c) else ConnectionsView(c)
        }
    }
}

@Composable
private fun SmallAction(label: String, onClick: () -> Unit) {
    Text(
        label, color = TextMuted, fontSize = 11.sp,
        modifier = Modifier.clickable(onClick = onClick).padding(horizontal = 8.dp, vertical = 7.dp),
    )
}

@Composable
private fun LogView(c: AppController) {
    val state = rememberLazyListState()
    LaunchedEffect(c.logs.size) {
        if (c.logs.isNotEmpty()) state.scrollToItem(c.logs.size - 1)
    }
    LazyColumn(Modifier.fillMaxSize().padding(horizontal = 8.dp, vertical = 4.dp), state = state) {
        items(c.logs.size) { i ->
            val line = c.logs[i]
            Text(
                line,
                color = when {
                    line.contains("error", true) || line.contains("失败") -> Danger
                    line.contains("warn", true) -> Warn
                    else -> TextMuted
                },
                fontSize = 11.sp,
                fontFamily = FontFamily.Monospace,
            )
        }
    }
}

@Composable
private fun ConnectionsView(c: AppController) {
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("目标", Modifier.weight(1f), color = TextFaint, fontSize = 11.sp)
            Text("出站链路", Modifier.width(220.dp), color = TextFaint, fontSize = 11.sp)
            Text("上行", Modifier.width(80.dp), color = TextFaint, fontSize = 11.sp)
            Text("下行", Modifier.width(80.dp), color = TextFaint, fontSize = 11.sp)
        }
        HorizontalDivider(color = BorderLight)
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 8.dp)) {
            items(c.connections) { conn ->
                Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        conn.host.ifBlank { "—" }, Modifier.weight(1f),
                        color = TextPrimary, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        conn.chains, Modifier.width(220.dp), color = TextMuted, fontSize = 11.sp,
                        maxLines = 1, overflow = TextOverflow.Ellipsis,
                    )
                    Text(fmtRate(conn.upload), Modifier.width(80.dp), color = TextMuted, fontSize = 11.sp)
                    Text(fmtRate(conn.download), Modifier.width(80.dp), color = TextMuted, fontSize = 11.sp)
                }
            }
            if (c.connections.isEmpty()) {
                item {
                    Text(
                        if (c.running) "暂无活动连接" else "内核未运行",
                        color = TextFaint, fontSize = 11.sp, modifier = Modifier.padding(8.dp),
                    )
                }
            }
        }
    }
}

// ---------------------------------------------------------------- 状态栏

@Composable
private fun StatusBar(c: AppController) {
    val proxied = c.connections.count { !it.chains.contains("direct") }
    val direct = c.connections.size - proxied

    Row(
        Modifier.fillMaxWidth().background(ToolbarBg).padding(horizontal = 10.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(if (c.running) "运行中" else "未启动", color = if (c.running) Success else TextMuted, fontSize = 11.5.sp)
        Spacer(Modifier.width(24.dp))
        Text("Socks/HTTP: 127.0.0.1:${c.settings.mixedPort}", color = TextMuted, fontSize = 11.5.sp)
        Spacer(Modifier.width(16.dp))
        Text("Clash: 127.0.0.1:${c.settings.clashPort}", color = TextMuted, fontSize = 11.5.sp)
        Spacer(Modifier.weight(1f))
        Text("代理: $proxied", color = TextMuted, fontSize = 11.5.sp)
        Spacer(Modifier.width(14.dp))
        Text("直连: $direct", color = TextMuted, fontSize = 11.5.sp)
        Spacer(Modifier.width(14.dp))
        Text("↑ ${fmtRate(c.up)}/s  ↓ ${fmtRate(c.down)}/s", color = TextMuted, fontSize = 11.5.sp)
    }
}

private fun fmtRate(bytes: Long): String = when {
    bytes >= 1024 * 1024 -> "%.1f MB".format(bytes / 1024.0 / 1024.0)
    bytes >= 1024 -> "%.1f KB".format(bytes / 1024.0)
    else -> "$bytes B"
}
