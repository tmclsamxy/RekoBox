# 账号与节点池（Windows 客户端 + 自托管服务端）

目标：
- 管理员在一张网页上维护**共享节点池**（按分享链接录入，如 `tuic://…`）；
- 客户端登录后自动拉取，**只读**——没有任何上传通道，用户之间不会互相改坏节点；
- 不登录也能正常使用本地节点；断网时本地照常读写，恢复后自动补拉。

```
┌───────────────────────────┐   浏览器    ┌───────────────────────────┐
│ 管理员                    │ ─────────► │ rekosync (Go, 标准库)      │
│  http://host:8080/admin   │  Bearer    │                           │
└───────────────────────────┘            │ data/users.json           │
                                         │ data/tokens.json          │
┌───────────────────────────┐            │ data/nodes.json  ← 节点池  │
│ RekoBox Windows (Qt6)     │  GET only  │                           │
│  sync/SyncEngine          │ ─────────► │ 没有客户端写接口           │
│   ├ SyncStore  state.json │ ◄───────── │ PUT /vault 固定 403        │
│   └ SyncApi    Qt Network │  节点数组   │                           │
└───────────────────────────┘            └───────────────────────────┘
```

---

## 1. 权限模型

| 角色 | 登录网页控制台 | `GET /api/v1/nodes` | 管理接口 | 上传 |
|---|---|---|---|---|
| `admin` | ✅ | ✅ | ✅ | ❌ |
| `user` | ❌ | ✅ | ❌（403） | ❌ |

- 账号不开放注册，全部由 CLI 或控制台创建（`-addadmin` / `-adduser`）。
- 令牌是 32 字节随机数的 base64url，服务端只存 SHA-256；`expires_at = 0` 表示不过期。
- 令牌失效的四种情况：用户注销、管理员改口令、管理员禁用/删号。
- 客户端拿到 401 时清会话退回本地模式，节点数据不动，界面提示重新登录。

**为什么没有上传**：早期版本是「每账号一份节点快照，双向同步」。多人共用时这个模型必然
互相踩——一个用户改名/删节点就会影响别人。现在改成单一节点池由管理员维护，
客户端只有拉取权，从根本上消除了越权改动。

---

## 2. 服务端接口

请求/响应均为 `application/json; charset=utf-8`，错误统一：

```json
{"error": {"code": "bad_credentials", "message": "用户名或口令错误"}}
```

### 2.1 客户端用

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/v1/health` | 否 | 探活 `{ok,name,version,time,uptime_sec}` |
| POST | `/api/v1/auth/login` | 否 | `{username,password,device}` → `{token,expires_at,user{name,role},nodes{count,read_only}}` |
| POST | `/api/v1/auth/logout` | 是 | 吊销当前令牌 |
| GET | `/api/v1/auth/me` | 是 | 账号 + 节点池 `{version,updated_at,count}` |
| GET | `/api/v1/nodes` | 是 | 启用中的节点，按管理员排序 |
| GET | `/api/v1/nodes?version=N` | 是 | 版本未变时只回 `{unchanged:true}`，省流量 |
| PUT/GET | `/api/v1/vault` | 是 | 旧协议兼容：PUT 固定 **403**，GET 固定 204 |

`GET /api/v1/nodes` 响应：

```json
{
  "version": 12,
  "updated_at": 1794473678040,
  "count": 2,
  "read_only": true,
  "nodes": [
    {"id":"d9c0b6c156f9","name":"香港-01","type":"tuic",
     "addr":"203.0.113.10","port":25247,"group":"香港","remark":"示例备注",
     "link":"tuic://00000000-1111-2222-3333-444444444444:password@203.0.113.10:25247?congestion_control=bbr#..."}
  ]
}
```

`link` 是唯一事实来源，客户端用它还原完整节点参数；其余字段只用于识别与展示。

### 2.2 管理端用

前缀 `/api/v1/admin/`，全部要求 `role=admin`：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `overview` | 服务端版本/运行时长、节点总数与启用数、账号数 |
| GET | `nodes` | 全量节点（含禁用） |
| POST | `nodes` | `{links:"…\n…", group, remark}` 批量录入，逐条返回成功/拒绝原因 |
| PATCH | `nodes/{id}` | `{name?,group?,remark?,enabled?,link?}` |
| DELETE | `nodes/{id}` | 删一条 |
| POST | `nodes/batch` | `{ids:[…], action:"enable"\|"disable"\|"delete"}` |
| POST | `nodes/order` | `{ids:[…]}` 按给定顺序重排 |
| GET/POST | `users` | 列表 / `{name,password,role}` 建号 |
| DELETE | `users/{name}` | 删号（连带吊销令牌） |
| POST | `users/{name}/password` | `{password}`（吊销该账号旧令牌） |
| POST | `users/{name}/state` | `{disabled:true\|false}`（禁用即吊销令牌） |

不能删除/禁用"当前正在使用的管理员账号"。

### 2.3 节点链接解析

服务端只做最小解析，用于展示、排序与去重：协议、地址、端口、`#名称`。
不解析具体协议参数——加新协议不用改服务端，客户端用自己的解析器还原即可。

放行的协议（与客户端导入能力一致）：

```
vmess://  vless://  trojan://  ss://  socks(4/4a/5)://  http(s)://
tuic://  hysteria2://  hy2://  naive+https://  naive+quic://
```

- 重复链接（完全相同的字符串）拒绝，返回 `该链接已在节点池中`。
- 同名节点自动加序号 `(2)`，避免列表里一堆同名条目。
- 非法协议/缺 host 或 port 的链接直接拒绝并给出原因。

---

## 3. 网页控制台

地址 `http://<服务器>:8080/admin`，单文件 HTML（`go:embed` 进二进制，无 CDN 依赖）。

| 区域 | 功能 |
|---|---|
| 概览卡片 | 节点总数、启用数、账号数、节点池版本与更新时间 |
| 节点管理 | 搜索（名称/地址/分组/备注）、分组与状态筛选、批量启停/删除、上下移排序、编辑、复制链接 |
| 添加节点 | 多行粘贴链接，可带分组/备注；逐条显示成功与拒绝原因 |
| 账号管理 | 建号（普通/管理员）、改口令、禁用/启用、删除；显示在线设备数与最后活动时间 |
| 接入说明 | 客户端填法、协议清单、接口一览 |

安全：

- 令牌存 `localStorage`（可取消"记住登录"），请求走 `Authorization: Bearer`，
  不用 Cookie → 没有 CSRF 面；
- 所有管理接口二次校验角色，前端隐藏只是体验，不作为权限依据；
- 页面无外部资源，`X-Content-Type-Options: nosniff`、`Referrer-Policy: same-origin`。

---

## 4. 客户端（Windows）

### 4.1 一次拉取

```
1. GET /api/v1/nodes?version=<上次版本>
2. unchanged → 结束
3. 逐条对照（key = 服务端节点 id → 本地实体 sync_id = "srv:<id>"）：
   · 本地没有            → 解析 link，新建实体到对应分组
   · 有、链接指纹未变     → 不动（本地改名/调参不会被覆盖）
   · 有、链接指纹变了     → 同协议只换 bean 内容并保留本地 id/gid；
                           协议变了则删旧建新（正在运行则跳过这一轮）
4. 服务端已删除/禁用的（本地有、服务端列表里没有）：
   · 没在运行 → 删除本地实体与落地记录
   · 正在运行 → 保留，日志提示，下一轮再删
5. 分组内顺序按服务端顺序写一次；写 state.json / nodes.json
```

本地实体上的 `sync_id` 固定为 `srv:<id>`，所以反复拉取不会产生重复节点。

### 4.2 触发时机

| 时机 | 说明 |
|---|---|
| 登录成功后 | 立即拉一次 |
| 启动后 3 秒 | 等主窗口铺好列表再拉 |
| 定时 | 默认每 30 分钟（可选 15 分钟 ~ 12 小时，可关闭） |
| 手动 | 对话框「立即拉取」 |

### 4.3 本地对拉下来的节点做了什么

- 按服务端 `group` 字段自动建分组，分组 `sync_id = "srvgrp:<名称>"`；
  `group` 为空时落到「云端节点」。
- 可以本地改名、改端口、调参数——**只影响本机**，不会被上传，也不会影响别人。
- 服务端下次改了这条节点的链接才会覆盖它。
- 删除服务端节点后，客户端下次拉取会把本地那条删掉（正在用时延后）。

### 4.4 离线与错误

| 场景 | 行为 |
|---|---|
| 未登录 | 全功能本地使用，不产生任何网络请求 |
| 断网启动 | 正常读本地配置；拉取失败进入 `Offline`，5s→10s→…→5min 退避重试 |
| 服务端 5xx | 同样退避重试，不弹窗打扰 |
| 令牌失效（401） | 清会话退回本地模式，本地节点不动，提示重新登录 |
| 服务端下发了无法解析的链接 | 跳过该条并记日志，其余照常应用 |

### 4.5 本地文件

```
config/sync/state.json   服务器、账号、令牌、角色、上次版本/时间/节点数、自动拉取设置
config/sync/nodes.json   「服务端节点 id → 本地 sync_id + 上次应用的链接指纹」
```

令牌明文落盘（普通桌面程序做法）。磁盘可能被他人接触时请开 BitLocker。

---

## 5. 部署

### 一键部署（一条命令）

服务器上任意目录执行：

```bash
curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh | sudo bash
# 交互不出现时改用：sudo bash <(curl -fsSL <同一地址>)
```

脚本是交互菜单，三个动作：

| 选项 | 行为 |
|---|---|
| 1 安装 | 询问服务端口、是否只监听本机、安装目录、管理员账号与口令、是否放行防火墙；装依赖 → 装 Go（缺）→ 拉源码 → 编译 → systemd → 建管理员 → 自检 |
| 2 更新 | 拉最新源码重编译，保留 `data/`（账号与节点池）；新版本起不起来会自动回滚 `rekosync.prev` |
| 3 清理卸载 | 停服务 → 删单元文件 → 删程序与数据 → 删源码缓存 → 删系统用户/组 → 移除防火墙规则 → 残留校验；删干净后可立即重装 |

安装位置会记到 `/etc/rekobox-install.conf`，之后更新/卸载不用重复 `--dir`。
卸载默认删除数据（账号/令牌/节点池），要留档用 `--keep-data`（先打包到 `/var/backups`）。
脚本化用法：`--install -y`、`--update`、`--uninstall -y`，详见 `server/README.md`。

完成后打印：

```
网页控制台  http://<公网IP>:<端口>/admin
客户端填    http://<公网IP>:<端口>
```

### 手动

```bash
GOFLAGS=-mod=mod GOPROXY=https://goproxy.cn,direct CGO_ENABLED=0 \
  go build -trimpath -ldflags "-s -w" -o rekosync .      # 单文件约 5-6MB

./rekosync -data /opt/rekosync/data -addadmin root       # 建管理员
./rekosync -data /opt/rekosync/data -adduser  alice      # 建普通用户
./rekosync -data /opt/rekosync/data -users
./rekosync -data /opt/rekosync/data -passwd  alice       # 踢掉旧令牌
./rekosync -data /opt/rekosync/data -disable alice
```

### 对外开放

- 直接用 HTTP：`-addr :8080`，安全组/`ufw` 放行 8080。
- 想加 TLS：`-addr 127.0.0.1:8080` + nginx 反代，客户端地址改成 `https://` 即可，
  服务端代码不用改。
- **HTTP 明文意味着口令与节点链接在链路上可见**，公网长期使用请务必上 TLS。

### 备份 / 迁移

```bash
tar czf rekosync-$(date +%F).tgz -C /opt/rekosync data
```

`data/` 是全部状态（账号、令牌、节点池），拷走即迁移完成。

---

## 6. 客户端使用

1. 启动 `rekobox.exe`。
2. 菜单「首选项」→「账号与节点订阅」。
3. 填服务器地址（`1.2.3.4:8080` 或 `http://1.2.3.4:8080`）、账号、口令 → 登录。
4. 登录后自动拉取；之后按 §4.2 的时机更新。
5. 想停掉自动更新：取消「自动拉取节点」，或用下拉框改成更大的间隔。

---

## 7. 已知限制

1. **单进程服务**：账号/令牌/节点池缓存在内存，多开同一 `data` 目录会互相覆盖。
2. **改端口等于换链接**：服务端只按 `link` 判断有没有变化，编辑节点链接才会推给客户端。
3. **本地改动不上传**：这是设计目标，不是缺陷。需要共享的改动请交给管理员。
4. **不同端的本地改动互不影响**：同一账号在 Windows 与 Android 上各自可改本地副本，
   下一次服务端改了这条链接才会覆盖回去。
5. **旧快照留着不删**：`data/vaults/*.json` 不再读写，确认不需要后可手工删除。
6. **节点链接含明文密钥**：`data/nodes.json` 里是明文，磁盘请做加密或严格权限
   （默认 0600/0700）。

---

## 8. Android 端

安卓与 Windows 用**同一套协议、同一台服务器**，服务端代码无需改动；
订阅层同样改成只读拉取，入口：侧边抽屉 →「账号与节点池」。

### 8.1 与 Windows 的差异

| 关注点 | Windows | Android |
|---|---|---|
| 本地存储 | JSON 文件（`config/profiles/*.json`） | Room（`proxy_entities` / `proxy_groups`） |
| 节点标识 | 实体自带 `sync_id = "srv:<id>"` | 不改表结构，映射存在 `files/sync/nodes.json` |
| 会话 | `config/sync/state.json` | `SharedPreferences("rekosync")` |
| 定时拉取 | `QTimer`，可选 15 分钟~12 小时 | 协程定时，默认 30 分钟，开关在账号页 |
| 触发 | 登录后、启动 3s、定时、手动 | 同左（`AccountFragment` 里可手动拉取） |

### 8.2 拉取流程

```
GET /api/v1/nodes?version=<上次版本>
  unchanged → 结束
  逐条对照（key = 服务端节点 id，值 = {本地 profileId, 链接指纹}）：
    · 本地没有         → 解析 link 建节点到对应分组
    · 有、指纹未变      → 不动（本地改名/调参保留）
    · 有、链接变了      → 原地覆盖 bean，保留 profileId 与 userOrder
  服务端已删除/禁用 → 删除本地节点；正在使用的那条跳过，切走后再删
```

- 分组按服务端 `group` 字段匹配本地同名分组，没有就新建（空则落到「云端节点」）；
- 链接解析用 `LinkParser`：vmess / vless / trojan / ss / socks / http / hysteria2 / tuic / naive，
  与服务端放行清单一致；
- vmess 做了宽容处理：遇到只给 `add/port/id` 的极简 JSON 会补 `net/tcp` 等缺省字段再解析
  （部分面板的链接 Windows 能连、安卓内置解析器会拒绝）；
- 解析出的 bean 先 `initializeDefaultValues()` 再落库，否则 Kryo 序列化会在
  `Boolean.booleanValue()` 上空指针（tuic 的 `allowInsecure/disableSNI/fastConnect` 都是可空字段）；
- 单条节点失败（链接坏了 / 字段异常）只跳过这一条，不影响整轮；失败原因写日志。

### 8.3 本地文件

```
files/sync/nodes.json   服务端节点 id → {local_id, hash}
```

旧版的 `files/sync/meta.json`（墓碑、uuid 绑定、看不懂的远端条目）不再使用，
升级后可以忽略；账号、令牌都在 SharedPreferences 里，不随文件删除丢失。

### 8.4 已知差异

1. 分组没有稳定标识，只能按**名称**匹配：把服务端分组改名等于新建分组。
2. 不支持在客户端编辑节点后同步回服务端（本来也不允许上传）。
3. 只在 Android 11（x86_64 模拟器）上验证过拉取 / 原地更新 / 禁用删除；
   真机回归与 hysteria2 实际出网未覆盖。

## 9. 代码位置

| 文件 | 职责 |
|---|---|
| `server/nodes.go` | 节点池存储、分享链接解析、排序与去重 |
| `server/api.go` | 客户端接口：登录、`GET /nodes`、旧 vault 兼容、日志中间件 |
| `server/admin.go` | 管理接口分发、概览、账号管理、内嵌网页 |
| `server/web/admin.html` | 网页控制台（单文件，内联 CSS/JS） |
| `server/store.go` | 账号/令牌（PBKDF2、SHA-256 令牌键）、角色与禁用 |
| `server/main.go` | CLI 子命令、HTTP 启动、优雅退出 |
| `sync/SyncEngine.{hpp,cpp}` | 拉取状态机、链接→实体、落地与清理 |
| `sync/SyncStore.{hpp,cpp}` | `state.json` / `nodes.json` 读写 |
| `sync/SyncApi.{hpp,cpp}` | Qt Network 封装：Bearer、20s 超时、错误文案 |
| `sync/SyncModel.{hpp,cpp}` | `sync_id` 生成、时间戳、内容指纹 |
| `ui/dialog_account.{h,cpp}` | 「账号与节点订阅」对话框 |
| `RekoboxForAndroid/.../sync/SyncEngine.kt` | 安卓拉取状态机、落地与清理 |
| `RekoboxForAndroid/.../sync/SyncStore.kt` | Android 会话与 `nodes.json` |
| `RekoboxForAndroid/.../sync/LinkParser.kt` | 分享链接 → 节点 bean（含 vmess 宽容解析） |
| `RekoboxForAndroid/.../ui/AccountFragment.kt` | 「账号与节点池」界面 |
