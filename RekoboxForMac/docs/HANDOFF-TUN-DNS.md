# macOS TUN + DNS 交接文档

> 写于 2026-11-20 21:00，交接给新会话。代码状态：`main` 分支，本地已提交待推。
> 目标：让 macOS 的 TUN 模式可正常上网，并接管 DNS。

## 一句话现状

TUN 能建起来、路由能加、停用能清理，**但一开 TUN 整机就没网**。已定位到直接原因（见"硬证据"），
修复方向明确但**尚未实施**。DNS 接管代码已写好，但因为它依赖 TUN 可用，**没有验证通过**。

## 硬证据

`~/Library/Application Support/rekobox/config/neko.log` 里有 56 行这样的错误：

```
error connection upload closed: close tcp 172.19.0.1:57382->27.128.147.91:443:
      shutdown: can't assign requested address
error connection download closed: raw read: can't assign requested address
```

**关键**：出站 socket 的**本地地址是 `172.19.0.1`**，也就是 utun 自己的地址。

这排除了"包被内核路由进 tun（还能靠 `route_exclude_address` 挖出来）"这个解释——
socket 一旦**绑定到 utun 接口**，发出去的包直接注入 tun，路由表里怎么排除都没用。
tun 一关，这些 socket 的本地地址就失效，于是 `can't assign requested address`。

## 根因

`db/ConfigBuilder.cpp:760`（主内核配置，不是 tun 配置）：

```cpp
{"auto_detect_interface", dataStore->spmode_vpn},   // TUN 开着时为 true
```

`auto_detect_interface` 会让 sing-box 把**所有出站 socket 绑定到当前默认接口**。
Windows 上这么做是对的（单进程 tun，绑定物理网卡正好防回环）；
mac 是两进程模型，TUN 起来后默认接口变成 utun4，主内核于是把自己的出站也绑到了 utun4：

```
主内核连节点/连 DNS 上游 → socket 绑定 utun4 → 包进 tun → tun 内核 → socks → 主内核 → 再绑 utun4 → 死循环
```

这解释了为什么**有时候能上网、有时候不能**：取决于接口监视器什么时候把"默认接口"从 en0 切成 utun4。
之前的几次成功（19:47、20:39）是切换还没发生；一旦发生就全盘不通。

### 第二层问题（还没验证，但很可能存在）

即使关掉 `auto_detect_interface`，`bypass`（直连）出站在两进程模型下仍然无解：
主内核把要直连的流量发出去 → 被 tun 抓走 → tun 内核的规则匹配不到"这是主内核发的"
（`process_name` 规则在 darwin 上是空转的，`sing-tun@v0.7.10` 在 darwin 不提供 `ProcessInfo`）
→ 落到 `final: neko-socks` → 又回到主内核 → 再次直连 → 循环。

主内核日志里 `outbound/direct[bypass]` 的量很大（用户配置里国内域名走 bypass），
所以这个问题一旦暴露，影响面不小。

## 建议的下一步（按顺序做，每步都要能独立验证）

### 1. 先关掉主内核的 auto_detect_interface（mac + TUN）

`db/ConfigBuilder.cpp:760` 附近：

```cpp
bool autoDetect = dataStore->spmode_vpn;
#ifdef Q_OS_MACOS
// mac 是两进程模型：TUN 起来后默认接口会变成 utun，
// 主内核一旦把出站绑到 utun 就一定会回环（见日志里的 172.19.0.1 源地址）
autoDetect = false;
#endif
{"auto_detect_interface", autoDetect},
```

关掉后主内核走内核路由表，节点地址已被 `route_exclude_address` 挖到 en0
（`buildTunRouteExclude()`，当前排除：`127.0.0.0/8`、`169.254.0.0/16`、`224.0.0.0/4`、
`255.255.255.255/32`、`8.8.8.8/32`、`<节点IP>/32`）。

**验证**：开 TUN 后 `neko.log` 里不应再出现 `172.19.0.1:` 作为源地址；
`curl --noproxy '*' -s https://1.1.1.1` 应返回 301。

### 2. 再验证 bypass 是否环回

开 TUN，访问一个走 `bypass` 的国内站点（用户配置里国内域名走 bypass），看 `neko.log`：

- 若出现"同一个目标反复 inbound/outbound"→ 确认第二层问题存在
- 解决方向（按代价排序）：
  1. **给 TUN 内核加规则按源地址直连**：主内核的 socks 连接来自 127.0.0.1，但直连流量源地址是 en0 的 IP。
     可以在 tun 配置里加 `{"source_ip_cidr": ["<本机 en0 IP>/32"], "outbound": "direct"}`，
     把"主内核直连发出的包"识别出来直接放行（这个思路要看 sing-box 能否拿到 sniff 后的源地址，需实测）
  2. **放弃两进程模型**：mac 也做"单进程 tun"，即 `vpn_internal_tun = true`，但那样要求 App 本身以 root 跑，
     或者用 `SMAppService` 装一个特权助手（P4 本来就要做这个）
  3. 让 TUN 内核的 `final` 改成 `direct`，只有明确规则才走 proxy（会改变产品行为，不推荐）

### 3. DNS 接管（代码已就绪，等 TUN 通了才能验）

已经实现的部分：

- `sys/macos/MacPlatform.cpp` 的 `ApplyTunDNS(bool)`：把各网络服务 DNS 写成 `172.19.0.2`，
  原值存 `config/tun-dns-state.json`（原本走 DHCP 的存空列表，还原时写 `Empty`）
- `ProbeDNS("172.19.0.2", 53)`：发最小 DNS 查询比对事务 ID，**探通才切**，
  探不通就不切（宁可没接管，不能让整机解析挂掉）
- `RecoverTunDNSIfNeeded()`：启动时还原上次异常退出留下的改动
- `ui/mainwindow.cpp` 的 `StartVPNProcess()` 里：1.2s 后开始探活，最多 4 次，失败写警告日志

地址为什么是 `172.19.0.2`：和安卓端一致（`VpnService.kt:28` 的 `PRIVATE_VLAN4_ROUTER = "172.19.0.2"`，
`addDnsServer` 用的就是它）。它落在 tun 的 `172.19.0.1/28` 里，靠默认路由进 tun，
被 tun 配置里 `{"port": 53, "outbound": "dns-out"}` 规则劫持。

**⚠️ 不能用的两个地址**（都实测过）：

- `172.19.0.1`（tun 接口自己）：内核本地投递，包不进 utun
- `127.0.0.1`：需要另起监听；且 sing-box 的 `dns-direct` 走"系统 DNS"，会变成自己查自己

**⚠️ 前置条件**：`route_exclude_address` 里**绝对不能**包含 `172.16.0.0/12`。
这条列表曾经包含它，结果 `172.19.0.2` 被排除出 tun，DNS 查询根本不进隧道，
表现为"接管后整机解析全挂"。踩过一次。

## 已验证 OK 的部分（不要重复踩）

| 项 | 状态 | 说明 |
|---|---|---|
| TUN 建 utun / 加路由 | ✅ | `utun4`、`172.19.0.1/28`、72 条路由指向 utun |
| TUN 停用清理 | ✅ | utun 消失、路由清零、root 内核退出、无 `172.19` 残留 |
| UDP 回程 panic | ✅ 已修 | `go/singbox-neko-headroom` + `go/patches/route-conn-headroom.patch` |
| 规则集离线加载 | ✅ 已修 | 13 个 `.srs` 打进 `Contents/MacOS/`，`build_macos.sh` 有三级回退下载 |
| 日志面板刷屏 | ✅ 已修 | TUN 开着时面板只放行 error/warn/`[mac]`/`[tun]`，完整日志仍进 `neko.log` |
| TUN 内核日志可见 | ✅ 已加 | 全量写 `config/neko-tun.log`（之前是黑洞，白排查了半天） |
| 退出崩溃 / 崩溃后打不开 | ✅ 已修 | `aboutToQuit` 不再 `waitForFinished`；mac 改用 `QLocalServer` 单实例 |

## 复现与取证命令

```bash
# 开 TUN 后立刻抓现场
networksetup -getdnsservers Wi-Fi                       # 应变成 172.19.0.2
ifconfig | grep -A1 utun | grep 172.19                  # tun 是否建起来
route -n get 172.19.0.2 | grep interface                # 必须是 utun
route -n get 8.8.8.8 | grep interface                   # 必须是 utun
route -n get <节点IP> | grep interface                   # 必须是 en0（被排除）
dig +short @172.19.0.2 www.google.com                   # 应有应答
curl --noproxy '*' -s -o /dev/null -w '%{http_code}\n' https://1.1.1.1

# 关键：看主内核出站的源地址
grep -c "172.19.0.1:" ~/Library/Application\ Support/rekobox/config/neko.log
#   >0 且还在增长 = 还在绑 tun（第 1 步没生效）

# TUN 内核自己的输出
tail -40 ~/Library/Application\ Support/rekobox/config/neko-tun.log
```

## 关键文件

| 文件 | 作用 |
|---|---|
| `db/ConfigBuilder.cpp` | 主内核配置 `BuildConfigSingBox`（`:760` auto_detect_interface）；tun 配置 `WriteVPNSingBoxConfig`（`:780` 起，含 `buildTunRouteExclude`） |
| `res/vpn/sing-box-vpn.json` | TUN 配置模板（tun 入站、dns 段、路由规则） |
| `sys/macos/MacPlatform.cpp` | `TunDNSUpstream()` / `ProbeDNS()` / `ApplyTunDNS()` / `RecoverTunDNSIfNeeded()` |
| `ui/mainwindow.cpp` | `StartVPNProcess()` / `StopVPNProcess()`（提权、日志捕获、DNS 探活） |
| `libs/build_macos.sh` | 构建 + 规则集下载 + dmg |
| `libs/apply_neko_patch.sh` | overlay + 补丁（幂等，每次构建都跑） |

## 相关提交

```
a8aa760 撤掉 TUN 改系统 DNS 的尝试（方案不成立）
d1470a4 退出时崩溃 + 崩溃后打不开
0167b06 TUN 模式跑通（提权分支、回环、UDP 回程 panic）
15cd321 P3 打包 dmg + CI
```

`0167b06` 里的"回环"修复只解决了**路由层面**，没有解决**接口绑定层面**——这正是本次卡住的地方。
