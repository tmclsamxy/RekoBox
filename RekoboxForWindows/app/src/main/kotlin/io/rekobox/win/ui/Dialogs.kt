package io.rekobox.win.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.DialogWindow
import androidx.compose.ui.window.rememberDialogState
import io.rekobox.core.AppPaths
import io.rekobox.core.model.EngineSettings
import io.rekobox.core.model.RunMode
import io.rekobox.core.model.Subscription
import io.rekobox.win.AppController
import io.rekobox.win.Sheet
import io.rekobox.win.label
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private val timeFmt = DateTimeFormatter.ofPattern("MM-dd HH:mm").withZone(ZoneId.systemDefault())

@Composable
private fun accentButton() = ButtonDefaults.buttonColors(containerColor = Accent, contentColor = Color.White)

@Composable
private fun DialogFrame(title: String, width: Int, height: Int, onClose: () -> Unit, content: @Composable () -> Unit) {
    DialogWindow(
        onCloseRequest = onClose,
        title = title,
        state = rememberDialogState(size = DpSize(width.dp, height.dp)),
        resizable = true,
    ) {
        RekoboxTheme {
            Box(Modifier.fillMaxSize().background(Bg)) { content() }
        }
    }
}

// ================================================================ 导入

@Composable
fun ImportDialog(c: AppController) {
    var text by remember { mutableStateOf("") }
    val clipboard = LocalClipboardManager.current
    val close = { c.sheet = Sheet.NONE }

    DialogFrame("导入节点", 660, 460, close) {
        Column(Modifier.fillMaxSize().padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(
                "一行一个分享链接，也可以直接粘贴订阅返回的内容（整段 base64 会自解码）。",
                color = TextMuted, fontSize = 12.sp,
            )
            Text(
                "支持 ss / vmess / vless / trojan / hysteria2 / tuic / anytls / socks / http，或 sing-box outbound JSON。",
                color = TextFaint, fontSize = 11.5.sp,
            )
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                placeholder = { Text("ss://…\nvmess://…\ntrojan://…", fontSize = 12.sp) },
                modifier = Modifier.weight(1f).fillMaxWidth(),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                Button(
                    onClick = { c.importText(text); text = ""; close() },
                    colors = accentButton(),
                    enabled = text.isNotBlank(),
                ) { Text("导入") }
                OutlinedButton(onClick = { text = clipboard.getText()?.text ?: "" }) { Text("从剪贴板粘贴") }
                OutlinedButton(onClick = { text = "" }) { Text("清空") }
                Spacer(Modifier.weight(1f))
                TextButton(onClick = close) { Text("关闭", color = TextFaint) }
            }
        }
    }
}

// ================================================================ 订阅

@Composable
fun SubscriptionsDialog(c: AppController) {
    var name by remember { mutableStateOf("") }
    var url by remember { mutableStateOf("") }
    val close = { c.sheet = Sheet.NONE }

    DialogFrame("订阅管理", 760, 560, close) {
        Column(Modifier.fillMaxSize().padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = name, onValueChange = { name = it },
                    label = { Text("备注名") }, singleLine = true, modifier = Modifier.width(170.dp),
                )
                OutlinedTextField(
                    value = url, onValueChange = { url = it },
                    label = { Text("订阅地址 https://…（也支持 file://）") }, singleLine = true,
                    modifier = Modifier.weight(1f),
                )
                Button(
                    onClick = { c.addSubscription(name, url); name = ""; url = "" },
                    colors = accentButton(),
                    enabled = url.isNotBlank(),
                ) { Text("添加并更新") }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedButton(onClick = { c.refreshAllSubscriptions() }, enabled = c.subscriptions.isNotEmpty()) {
                    Text("全部更新")
                }
                Text("${c.subscriptions.size} 个订阅 · 重复拉取按链接去重", color = TextFaint, fontSize = 11.5.sp)
                Spacer(Modifier.weight(1f))
                TextButton(onClick = close) { Text("关闭", color = TextFaint) }
            }

            HorizontalDivider(color = Surface2, modifier = Modifier.padding(vertical = 2.dp))

            LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                items(c.subscriptions, key = { it.id }) { sub -> SubscriptionRow(c, sub) }
                if (c.subscriptions.isEmpty()) {
                    item { Text("还没有订阅", color = TextFaint, fontSize = 12.sp, modifier = Modifier.padding(12.dp)) }
                }
            }
        }
    }
}

@Composable
private fun SubscriptionRow(c: AppController, sub: Subscription) {
    Row(
        Modifier.fillMaxWidth().background(Surface1, RoundedCornerShape(8.dp)).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(sub.name, color = TextPrimary, fontSize = 13.sp, fontWeight = FontWeight.Medium)
                if (sub.lastError != null) Badge("异常", Warn, filled = true)
                else Badge("${sub.lastCount} 节点", Accent, filled = true)
            }
            Text(sub.url, color = TextFaint, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
            Text(
                sub.lastError ?: if (sub.lastUpdate > 0) "上次更新 " + timeFmt.format(Instant.ofEpochMilli(sub.lastUpdate)) else "从未更新",
                color = if (sub.lastError != null) Warn else TextFaint,
                fontSize = 11.sp,
            )
        }
        TextButton(onClick = { c.refreshSubscription(sub) }) { Text("更新", fontSize = 12.sp) }
        TextButton(onClick = { c.removeSubscription(sub) }) { Text("删除", color = Danger, fontSize = 12.sp) }
    }
}

// ================================================================ 设置

@Composable
fun SettingsDialog(c: AppController) {
    var draft by remember(c.settings) { mutableStateOf(c.settings) }
    var portText by remember(c.settings) { mutableStateOf(c.settings.mixedPort.toString()) }
    var clashText by remember(c.settings) { mutableStateOf(c.settings.clashPort.toString()) }
    var dirty by remember { mutableStateOf(false) }
    val close = { c.sheet = Sheet.NONE }

    fun touch(block: (EngineSettings) -> EngineSettings) {
        draft = block(draft)
        dirty = true
    }

    DialogFrame("设置", 720, 660, close) {
        Column(Modifier.fillMaxSize().padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(12.dp)) {

                SectionCard("运行模式") {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        ModeChip("TUN", "接管全局，需管理员", draft.runMode == RunMode.TUN) {
                            touch { it.copy(runMode = RunMode.TUN) }
                        }
                        ModeChip("系统代理", "改 WinINet，免提权", draft.runMode == RunMode.SYSTEM_PROXY) {
                            touch { it.copy(runMode = RunMode.SYSTEM_PROXY) }
                        }
                        ModeChip("仅本地", "只开 127.0.0.1 端口", draft.runMode == RunMode.MANUAL) {
                            touch { it.copy(runMode = RunMode.MANUAL) }
                        }
                    }
                }

                SectionCard("端口与 DNS") {
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        OutlinedTextField(
                            value = portText,
                            onValueChange = { s ->
                                portText = s.filter { it.isDigit() }.take(5)
                                portText.toIntOrNull()?.takeIf { it in 1..65535 }?.let { v -> touch { it.copy(mixedPort = v) } }
                            },
                            label = { Text("混合端口") }, singleLine = true, modifier = Modifier.width(160.dp),
                        )
                        OutlinedTextField(
                            value = clashText,
                            onValueChange = { s ->
                                clashText = s.filter { it.isDigit() }.take(5)
                                clashText.toIntOrNull()?.takeIf { it in 1..65535 }?.let { v -> touch { it.copy(clashPort = v) } }
                            },
                            label = { Text("控制端口") }, singleLine = true, modifier = Modifier.width(160.dp),
                        )
                    }
                    OutlinedTextField(
                        value = draft.remoteDns,
                        onValueChange = { s -> touch { it.copy(remoteDns = s) } },
                        label = { Text("代理解析 DNS") }, singleLine = true, modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = draft.directDns,
                        onValueChange = { s -> touch { it.copy(directDns = s) } },
                        label = { Text("直连 DNS") }, singleLine = true, modifier = Modifier.width(240.dp),
                    )
                }

                SectionCard("行为") {
                    ToggleRow("局域网与私有地址直连", draft.bypassLan) { v -> touch { it.copy(bypassLan = v) } }
                    ToggleRow("自动选择最优节点（URLTest）", draft.autoSelect) { v -> touch { it.copy(autoSelect = v) } }
                    ToggleRow("开机自启", draft.autostart) { v -> touch { it.copy(autostart = v) } }
                    ToggleRow("关闭窗口时最小化到托盘", draft.closeToTray) { v -> touch { it.copy(closeToTray = v) } }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("日志级别", Modifier.width(180.dp), color = TextMuted, fontSize = 12.5.sp)
                        listOf("warn", "info", "debug", "trace").forEach { level ->
                            ModeChip(level, null, draft.logLevel == level) { touch { it.copy(logLevel = level) } }
                        }
                    }
                }
            }

            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(
                    onClick = { c.updateSettings(draft); dirty = false; close() },
                    colors = accentButton(),
                    enabled = dirty,
                ) { Text("保存并应用") }
                if (dirty) Text("有未保存的改动", color = Warn, fontSize = 11.5.sp)
                Spacer(Modifier.weight(1f))
                TextButton(onClick = close) { Text("取消", color = TextFaint) }
            }
        }
    }
}

@Composable
private fun ModeChip(label: String, hint: String?, selected: Boolean, onClick: () -> Unit) {
    Column(
        Modifier
            .background(if (selected) Accent.copy(alpha = 0.18f) else Surface2, RoundedCornerShape(8.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 7.dp),
    ) {
        Text(label, color = if (selected) Accent else TextMuted, fontSize = 12.5.sp, fontWeight = FontWeight.Medium)
        if (hint != null) Text(hint, color = TextFaint, fontSize = 10.sp)
    }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.weight(1f), color = TextMuted, fontSize = 12.5.sp)
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

// ================================================================ 关于

@Composable
fun AboutDialog(c: AppController) {
    val close = { c.sheet = Sheet.NONE }
    DialogFrame("关于 Rekobox", 520, 380, close) {
        Column(Modifier.fillMaxSize().padding(20.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("Rekobox for Windows", color = TextPrimary, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
            Text("版本 1.0.0 · GPL-3.0", color = TextFaint, fontSize = 12.sp)
            HorizontalDivider(color = Surface2, modifier = Modifier.padding(vertical = 6.dp))
            KeyValueRow("内核", c.corePath?.toString() ?: "未找到 sing-box.exe")
            KeyValueRow("运行模式", c.settings.runMode.label())
            KeyValueRow("数据目录", AppPaths.dataDir.toString())
            KeyValueRow("配置文件", AppPaths.configFile.toString())
            Spacer(Modifier.weight(1f))
            Text(
                "协议栈来自 sing-box，界面参考 NekoRay 的桌面交互；Android 版基于 NekoBox for Android。",
                color = TextFaint, fontSize = 11.5.sp,
            )
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                Button(onClick = close, colors = accentButton()) { Text("关闭") }
            }
        }
    }
}
