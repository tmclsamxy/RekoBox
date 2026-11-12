# 账号与云端同步（Windows 客户端 + 自托管服务端）

目标：Windows 端登录后自动与自有服务器同步分组与节点配置，多设备保持一致；
不登录也能正常使用；断网时本地照常读写，恢复后自动补同步。

```
┌──────────────────────────┐         HTTP/1.1          ┌──────────────────────────┐
│ RekoBox Windows (Qt6)    │  Authorization: Bearer …  │ rekosync (Go, 标准库)     │
│                          │ ────────────────────────► │                          │
│ sync/SyncEngine          │   GET/PUT /api/v1/vault   │ data/users.json          │
│   ├ SyncStore  state.json│ ◄──────────────────────── │ data/tokens.json         │
│   │            meta.json │   version 乐观锁 / 409    │ data/vaults/<user>.json  │
│   ├ SyncApi    Qt Network│                           │                          │
│   └ SyncModel  合并算法   │                           └──────────────────────────┘
└──────────────────────────┘
```

---

## 1. 服务端接口

所有请求与响应都是 `application/json; charset=utf-8`，错误统一为：

```json
{"error": {"code": "bad_credentials", "message": "用户名或口令错误"}}
```

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/v1/health` | 否 | 探活：`{ok, name, version, time, uptime_sec}` |
| POST | `/api/v1/auth/login` | 否 | `{username, password, device}` → `{token, expires_at, user, server_time, vault_exists}` |
| POST | `/api/v1/auth/logout` | 是 | 吊销当前令牌 |
| GET | `/api/v1/auth/me` | 是 | 校验令牌，回带 `user` 与 `vault{version,updated_at,device}` |
| GET | `/api/v1/vault` | 是 | 下载快照；服务端没有数据时返回 **204** |
| PUT | `/api/v1/vault` | 是 | `{base_version, device, payload}`；成功 200，版本冲突 **409** |

### 1.1 登录

```bash
curl -X POST http://1.2.3.4:8080/api/v1/auth/login \
     -H 'Content-Type: application/json' \
     -d '{"username":"alice","password":"secret123","device":"MY-PC"}'
```

```json
{"token":"f2RdKO…vokY","expires_at":0,
 "user":{"name":"alice","created_at":1794473665307},
 "server_time":1794473672645,"vault_exists":true}
```

- `expires_at = 0` 表示**不过期**：符合「登录后除非主动注销就永久有效」。
  服务端不写 TTL，令牌一直有效，直到 `logout`、管理员改口令或删号。
- 口令用 PBKDF2-HMAC-SHA256（210000 轮、16 字节随机盐）存储，
  格式 `pbkdf2_sha256$210000$<salt-b64>$<hash-b64>`。
- 令牌是 32 字节随机数的 base64url，服务端只存它的 SHA-256，
  所以 `data/tokens.json` 泄露也无法直接冒用。
- 账号不存在时也会跑一次哈希，避免用响应时间探测账号。
- 登录失败按 IP 限速：10 分钟内 10 次，超出返回 429。
  成功登录会清零计数。

> 本程序**没有注册接口**，账号全部由管理员在服务器上用 CLI 创建（见 §4.2）。

### 1.2 快照

`payload` 是客户端生成的 JSON，服务端**不解析内容**，只维护版本号：

```json
{
  "version": 12,
  "updated_at": 1794473678040,
  "device": "MY-PC",
  "payload": {
    "schema": 1,
    "exported_at": 1794473677900,
    "groups":   [ {"id":"<uuid>","mtime":1794473000000,"data":{"name":"默认","order_ids":["<uuid>"]}} ],
    "profiles": [ {"id":"<uuid>","mtime":1794473000000,"group":"<group-uuid>",
                   "data":{"type":"vmess","name":"HK-01","bean":{…}}} ]
  }
}
```

- `id` 是**实体自带的 UUID（`sync_id`）**，不是本地 int id：
  两台设备各自新建节点时 int id 必然撞车，`-flag_reorder` 还会整体重排 id。
- 删除用**墓碑**表示：同结构但 `deleted: true` 且不带 `data`。
- 实体里本机相关的字段不同步：`id`、`gid`、`yc`（延迟）、`report`（测速报告）、
  `traffic`（流量统计）、`group.order`（换成 `order_ids` 的 UUID 列表）。

---

## 2. 鉴权方式

```
Authorization: Bearer <token>        # 主用
X-Auth-Token: <token>                # 兼容 curl 调试
```

- **永久令牌**：客户端把令牌存 `config/sync/state.json`，每次请求带上；
  重启、关机都不影响。只有三种情况失效：
  1. 用户点「注销」（客户端调 `/auth/logout` 并清本地会话）；
  2. 管理员 `-passwd` 改口令（服务端吊销该账号所有令牌）；
  3. 管理员 `-deluser` 删号（令牌与快照一起删）。
- 客户端在同步时若收到 **401**，自动清会话并退回本地模式，节点数据不动，
  界面提示「登录已失效，请重新登录」。
- 令牌明文落盘（普通桌面程序的做法）。若磁盘可能被他人接触，
  请开 BitLocker；也可以把 `config/` 整体放到加密卷上。

---

## 3. 上传 / 下载 / 冲突处理

### 3.1 一次同步的完整顺序

```
1. 扫本地 → 算每个条目指纹，和 meta.json 对比
     · 指纹变了        → mtime = now（这是本地改动）
     · meta 里没有      → 新条目，mtime = now（首次基线时给 1，见下）
     · meta 有但实体没了 → 生成墓碑（mtime = now）
2. GET /api/v1/vault      → 得到 remote{version, payload}
3. MergeVault(local, remote) → merged          （逐条目 LWW，见 3.2）
4. applyVault(merged)     → 把 merged 写回本地实体与 meta
5. merged 与 remote 内容一致？ → 结束，不制造新版本
   ？否 → PUT {base_version: remote.version, payload: merged}
6. PUT 返回 409 → 重新 GET，回到第 3 步；最多 3 轮
```

### 3.2 合并规则（`sync/SyncModel.cpp: MergeVault`）

逐条目、按 `id`（UUID）对齐，last-write-wins：

| 情况 | 结果 |
|---|---|
| 只有一边有 | 采用 |
| 两边都有、内容相同 | 保留较大的 `mtime`（避免下次又判成改动） |
| 两边都有、内容不同 | `mtime` 大的赢；相等时**本地赢**（保证幂等） |
| 一边是墓碑 | 与普通条目同样比 `mtime`：删除比编辑新 → 删掉；编辑比墓碑新 → 复活 |
| `mtime` 相同且一边是墓碑 | **删除优先**（否则「被远端删掉的节点正在本机运行」会每轮复活，永不收敛） |

要点：

- **删除也参与比较**，所以「A 设备删了节点、B 设备刚好改了这个节点」不会出现
  删完又复活、或改完被永久吞掉的死循环，最终结果只取决于谁最后动手。
- **首次基线**（`meta.json` 为空）时，本地已有条目的 `mtime` 统一记为 `1`：
  服务端有数据时以服务端为准，本地独有的节点保留并上传。
  这样「先在旧电脑攒了一堆节点，再登录」不会把云端更完整的列表覆盖掉。
- 冲突计数只用于提示（「合并 N 处冲突」），不需要用户手工介入。
- **全新设备**（本机没有任何节点、只有安装时自动建的空「默认」分组）首次同步时，
  这个占位分组不参与上报，服务端有分组就用服务端的替换掉它——
  否则每装一台新机器，云端就会多一个「默认」分组。
- 合并是**幂等**的：`merge(merge(a,b), b) == merge(a,b)`，
  已用 `sync/SyncModel` 的单元测试覆盖（16 条断言全过）。

### 3.3 为什么把合并放在客户端

服务端只做「带版本号的 blob + 乐观锁」，好处：

- 服务端不关心节点格式，加新协议（新 bean 字段）不需要改服务端；
- 冲突处理需要「本地视图」，只有客户端知道哪些条目真的被改过；
- 服务端逻辑少，出错面小，几百行就能审计完。

代价：两端同时改同一账号时会有一次 409 往返（毫秒级，用户无感）。

### 3.4 服务端并发

`PUT` 在**同一把写锁内**比较 `base_version` 与当前版本再落盘，
所以两台设备同时提交时只有一台成功，另一台必拿 409 + 服务端当前快照。
写盘走「临时文件 + fsync + rename」，断电不会出现半截 JSON。

---

## 4. 部署

### 一键部署（推荐）

开发机上执行，交互式完成「交叉编译 → 上传 → 建账号 → 装 systemd → 自检 → 打印客户端地址」：

```bash
cd RekoboxForWindows/server && bash deploy.sh
```

口令不回显、不落盘、不进 `ps`（通过 stdin 传给 `-adduser`）；留空自动生成 20 位随机口令，
只显示一次。服务器需 root 或免密 `sudo -n`。非交互用法见 `server/README.md`。

### 4.1 编译服务端

服务器上直接编（需 Go 1.21+，国内用 `goproxy.cn`）：

```bash
cd RekoboxForWindows/server
GOFLAGS=-mod=mod GOPROXY=https://goproxy.cn,direct \
  CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o rekosync .
# 单文件约 8.5MB，无运行时依赖
```

或在 Windows 开发机上交叉编译后上传：

```bash
GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o rekosync .
scp rekosync root@1.2.3.4:/opt/rekosync/
```

### 4.2 创建账号（没有注册接口）

```bash
# 交互式输口令（不回显需自行加 stty -echo，或直接用 -password）
/opt/rekosync/rekosync -data /opt/rekosync/data -adduser alice

# 或一步到位
/opt/rekosync/rekosync -data /opt/rekosync/data -adduser alice -password 'strong-pass'

/opt/rekosync/rekosync -data /opt/rekosync/data -users            # 列账号
/opt/rekosync/rekosync -data /opt/rekosync/data -passwd  alice    # 改口令（踢掉旧令牌）
/opt/rekosync/rekosync -data /opt/rekosync/data -deluser alice    # 删号（连带删快照）
```

用户名限制：小写字母/数字/`_-.`，2–32 位，不以符号开头（防路径穿越）。
口令至少 6 位。

### 4.3 systemd

```bash
useradd -r -s /usr/sbin/nologin rekosync
mkdir -p /opt/rekosync/data
cp rekosync /opt/rekosync/ && chown -R rekosync:rekosync /opt/rekosync
cp RekoboxForWindows/server/rekosync.service /etc/systemd/system/
# 首次必须先建账号（以 root 执行，服务用 rekosync 用户跑）
/opt/rekosync/rekosync -data /opt/rekosync/data -adduser alice
chown -R rekosync:rekosync /opt/rekosync/data
systemctl daemon-reload && systemctl enable --now rekosync
systemctl status rekosync --no-pager
journalctl -u rekosync -f           # 每个请求一行：方法 路径 状态码 耗时 IP
```

### 4.4 端口与防火墙

- 直接用 HTTP：`-addr :8080`，云厂商安全组/`ufw` 放行 8080。
- 想只让本机反代：`-addr 127.0.0.1:8080` + nginx。
  以后要上 HTTPS，nginx 加一段即可，客户端把地址改成 `https://` 开头就行
  （服务端代码不变）。
- **HTTP 明文传输意味着口令与节点配置在链路上可被看到**，
  仅建议在可信网络或已用 WireGuard/内网穿透兜底时使用；
  公网长期使用请务必加 TLS。

### 4.5 备份与迁移

```bash
tar czf rekosync-$(date +%F).tgz -C /opt/rekosync data
```

`data/` 是全部状态（账号、令牌、快照），拷走即迁移完成；
换机器后改 systemd 的 `-data` 路径即可。

### 4.6 客户端

1. 启动 `rekobox.exe`。
2. 顶栏「首选项」→「账号与同步」（也可从菜单进入）。
3. 填服务器地址（`1.2.3.4:8080` 或 `http://1.2.3.4:8080`）、用户名、口令 → 登录。
4. 登录成功后自动同步一次；之后：
   - 本地增删改节点/分组，5 秒防抖后自动上传；
   - 启动后 3 秒自动拉一次；
   - 也可手动点「立即同步」。
5. 断网时状态显示「离线，稍后自动重试」，恢复后按 5s→10s→…→5min 退避自动补上。

客户端文件位置（`-appdata` 或默认 exe 同目录）：

```
config/sync/state.json   服务器、账号、令牌、设备名、上次同步版本/时间
config/sync/meta.json    每个条目的内容指纹与 mtime、待同步墓碑
```

---

## 5. 离线、缓存与重连

| 场景 | 行为 |
|---|---|
| 未登录 | 全功能本地使用，不产生任何网络请求 |
| 登录后断网启动 | 正常读本地配置；同步失败进入 `Offline`，退避重试 |
| 断网期间改节点 | 直接落盘，`pending=true` 持久化；联网后自动上传 |
| 断网期间删节点 | 删除照常生效；下次同步时由「meta 有、实体无」生成墓碑上传 |
| 杀进程/断电 | `state.json`、`meta.json` 每步即时写（临时文件+rename），不丢状态 |
| 令牌失效（401） | 清会话退回本地模式，节点不动，提示重新登录 |
| 服务端 5xx | 与断网同样退避重试，不弹窗打扰 |
| 远端删掉的节点正在本机运行 | 不立即删除（内核还在用它），日志提示；停止代理后下一轮同步删除 |

「本地缓存」就是程序原本的 `config/profiles/*.json`、`config/groups/*.json`——
云端同步是叠加层，不是替代品，所以任何同步故障都不影响本地可用性。

---

## 6. 已知限制

1. **单进程服务**：内存里缓存账号/令牌/快照，多开同一 `data` 目录会互相覆盖。
   一个实例足够；要横向扩展需换数据库。
2. **不做逐字段合并**：同一节点的「名称」和「端口」被两台设备同时改，
   只有整条记录的赢家生效，输家的那次修改不回滚到字段级。
3. **时钟偏差**：`mtime` 取本机时间，两台机器差很多时冲突判定会偏向时间快的。
   可用 `w32tm /resync` 校准；偏差在秒级时无实际影响。
4. **注销会清基线**：注销期间的删除不会同步到其他设备（下次登录按「首次基线」
   处理，以服务端为准）。要删除请先登录再删。
5. **墓碑保留 30 天**：超过 30 天且已同步到服务端的删除记录会被清理；
   长期离线（>30 天）的设备重新上线时，可能把已删节点当作「本地新增」补回去。
6. 节点里的口令/密钥是**明文**存服务端 `data/vaults/*.json`，
   服务器磁盘请做加密或严格权限（文件 0600、目录 0700，本服务默认如此）。

---

## 7. 代码位置

| 文件 | 职责 |
|---|---|
| `sync/SyncModel.{hpp,cpp}` | 报文结构、逐条目合并、指纹（纯函数，可单测） |
| `sync/SyncStore.{hpp,cpp}` | `state.json` / `meta.json` 读写、会话清理 |
| `sync/SyncApi.{hpp,cpp}` | Qt Network 封装：Bearer 头、20s 超时、错误文案 |
| `sync/SyncEngine.{hpp,cpp}` | 状态机：登录/注销/同步/退避重试/应用远端数据 |
| `sync/SyncNotify.hpp` | db 层通知同步模块的唯一入口（避免循环包含） |
| `ui/dialog_account.{h,cpp}` | 「账号与同步」对话框 |
| `db/ProxyEntity.hpp`、`db/Group.hpp` | 新增 `sync_id`（UUID）字段 |
| `main/NekoGui_ConfigItem.hpp` | 新增 `callback_after_save`，用于感知本地改动 |
| `server/*.go` | 服务端：`store.go` 存储、`auth.go` 口令令牌、`api.go` 路由、`main.go` CLI |
