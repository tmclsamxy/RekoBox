# 「立即拉取」点击无效 — 根因与修复

> 执行时间：2026-11-21 12:30–12:50 · 机器：MacBook Air M2 · macOS 15.7.5 · arm64
> 触发场景：首选项 → 账号与节点订阅 → 点「立即拉取」
> 结论：**已修复并在真机验证。根因是增量版本号把手动拉取短路成了 `unchanged`。**

---

## 1. 现象

- 注销后重新登录，**第一次**自动拉取正常，节点能更新。
- 之后无论点多少次「立即拉取」，界面**毫无变化**：状态文字不动、"上次拉取"时间戳不动、节点列表不动。
- 期间不影响已有节点使用，所以看起来像"按钮失灵"而不是"请求失败"。

## 2. 定位过程（用实机数据，不靠猜）

### 2.1 状态文件的时间戳对不上

`~/Library/Application Support/rekobox/config/sync/`：

| 文件 | 修改时间 | 含义 |
|---|---|---|
| `nodes.json` | 12:23 | 最后一次真正应用节点 |
| `state.json` | 12:26 | 被写过，但 `last_sync_at` 仍是 **12:23** |

`state.json` 在 12:26 被写过，说明**请求确实发出去了**（`saveState()` 被执行），
但 `last_sync_at` 没变 —— 只有 `unchanged` 分支会这样。

### 2.2 服务端证实返回 unchanged

对服务端（`http://120.46.1.89:3842`，rekosync 2.1.0）实测：

```bash
# 带客户端缓存的版本号
curl -H "Authorization: Bearer <token>" ".../api/v1/nodes?version=12"
# → {"read_only":true,"unchanged":true,"updated_at":...,"version":12}

# 不带 version
curl -H "Authorization: Bearer <token>" ".../api/v1/nodes"
# → version=12 count=5，5 条节点完整下发
```

服务端逻辑（`server/api.go` `handleNodes`）：`?version=` 与当前版本相等时直接回
`unchanged:true`，**不附带节点数组**。这是合理的省流量设计，本身没 bug。

### 2.3 客户端把 unchanged 当成了"什么都没发生"

原 `SyncEngine::runPull`：

```cpp
if (r.body.value("unchanged").toBool()) {
    finishOk(PullStats{}, true);   // 只标记"已是最新"
    return;
}
```

原 `SyncEngine::finishOk`：`unchanged == true` 时**不更新 `lastSyncAt`**。

所以整条链路是：用户点击 → 带 `version=12` 请求 → 服务端回 unchanged →
客户端什么都不做 → 界面零反馈。**"点了没反应"是准确描述，不是错觉。**

### 2.4 为什么"注销后第一次"会生效

`SyncStore::clearSession()` 会把 `lastVersion` 清零。重新登录后首次拉取不带
`version` 参数 → 服务端走全量分支 → 节点真正落地。此后 `lastVersion` 变成 12，
之后每次手动点击都撞上 `unchanged` 短路。

**这就是"只有第一次生效"的确切原因。**

## 3. 附带发现的一个隐患

若服务端有**解析不了的节点**，原 `applyNodes` 会 `alive.insert(id)` 后 `skipped++` 跳过，
本地不建实体，但 `state.nodes` 里的旧引用还在。

本来这只是个"跳过"的无害分支。但修复"缺节点自动补拉"之后，它会让
`HasMissingLocalNodes()` **恒为真** → unchanged → 强制重拉 → 又 unchanged → 无限循环。

**所以必须一并收敛记录**，否则修复本身会变成新的故障。

## 4. 改动清单

| 文件 | 改动 |
|---|---|
| `sync/SyncEngine.hpp` | `syncNow(reason, force=false)`、`runPull(attempt, force=false)`；新增 `HasMissingLocalNodes()`；新增 `queuedForce_`、`lastRepairAt` |
| `sync/SyncEngine.cpp` | 手动拉取强制全量；`unchanged` 也刷新 `lastSyncAt`；本地缺节点时自动强制补拉（30 s 节流）；排队请求保留 force 位；解析失败的孤儿引用直接清除 |
| `ui/dialog_account.cpp` | 「立即拉取」改调 `syncNow("manual", true)` |

### 4.1 关键实现

**强制全量**：不带 `version` 参数，服务端必定走全量分支，applyNodes 真正执行。

```cpp
// force：不带 version，服务端必定走全量分支
if (!force && state.lastVersion > 0) path += QStringLiteral("?version=%1").arg(state.lastVersion);
```

**unchanged 也刷新确认时间**：这样用户点击必有反馈，界面不再"装死"。

```cpp
if (unchanged) {
    // 服务端确认版本没变，等于本次拉取有效：刷新确认时间
    state.lastSyncAt = NowMs();
}
```

**本地缺节点自动修复**：用户手动删过节点、或配置目录被清过时，服务端一直说
unchanged 会导致永远补不回来。检出缺口就强制重拉，并用 30 s 节流防循环：

```cpp
const bool throttled = NowMs() - lastRepairAt < 30000;
if (HasMissingLocalNodes() && !throttled) {
    lastRepairAt = NowMs();
    runPull(0, true);   // 直接重跑，pulling_ 仍为 true，不产生额外并发
    return;
}
```

**孤儿引用收敛**：解析失败的节点如果本地没有实体，直接丢掉引用。

```cpp
if (!ent) state.nodes.remove(id);   // 否则 HasMissingLocalNodes() 恒为真
```

## 5. 验证结果

编译：`ninja` 通过（仅既有 Qt 弃用警告）。替换 `/Applications/RekoBox.app`
后重新 ad-hoc 签名，`codesign --verify --deep --strict` 通过。

真机连续点击「立即拉取」5 次，应用日志（`~/Library/Logs/Rekobox/app.log`）：

```
[12:47:41.562] [DEBUG] [sync] pull start: "manual" (force)
[12:47:41.594] [DEBUG] [sync] applied: created 0 updated 0 removed 0 skipped 0
[12:47:41.594] [DEBUG] [sync] done: "已是最新（5 个节点）"
... 连续 5 次，每次都是 (force) + applied
```

对比修复前的启动自动拉取：`pull start: "startup"`（无 force）→ 走 unchanged 分支。
修复后 `last_sync_at` 逐次推进，最终停在最后一次点击的 `12:47:44.539`。

`applied: created 0 updated 0 removed 0 skipped 0` 是**正确结果**：节点池确实没变，
应当零变更。关键区别是它现在**真的执行了应用流程**，而不是被短路。

## 6. 行为对照

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 点「立即拉取」（节点池无变化） | 带 `?version=12` → unchanged → 界面零反馈 | 强制全量 → 执行 applyNodes → 时间戳刷新 |
| 点「立即拉取」（节点池有变化） | 同上，且**拿不到新节点**（版本可能已变则能拿到，未变则拿到 unchanged） | 必定全量拉到最新 |
| 本地手动删过节点 | 永远补不回来（服务端持续 unchanged） | 检出缺口后自动强制补拉，30 s 节流 |
| 服务端有解析不了的节点 | skipped 后留孤儿引用 | 引用收敛，不触发重拉循环 |
| 自动/定时拉取 | 增量（省流量） | **保持增量不变**（未改自动路径） |

自动拉取仍然走增量，只有用户显式点击才强制全量 —— 流量开销可控。

## 7. 残留事项

- `/Applications/RekoBox.app.bak` 是替换前的备份，确认新版无问题后可删。
- 替换过程中产生了一个 root 权限的孤儿内核进程（父进程被 SIGTERM 后未回收）。
  已核实**无害**：不监听端口、不接管路由（默认路由仍在 `en0`）、系统代理全关、
  实测 `baidu/google` 均 200。它在下次重启或用户手动停 TUN 时自然消失。
  普通权限无法结束 root 进程，需要 `sudo`——不影响使用，故未处理。