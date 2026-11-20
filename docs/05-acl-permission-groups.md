# 节点权限组（服务端 ACL 升级）

在 v2.0 的「单一节点池 + 只读下发」基础上，给节点下发加上**按账号鉴权**：
管理员建权限组 → 给组配可访问的节点 → 把组分配给账号 → 客户端拉取时只拿到本组节点。

```
                    ┌──────────── 管理面（role=admin）────────────┐
管理员 ──/admin──►  │ 节点池 nodes.json                          │
                    │ 权限组 acl.json     group.nodes = [节点id]  │
                    │ 账号   users.json   user.groups = [组id]    │
                    └──────────────────┬──────────────────────────┘
                                       │ 鉴权：nodeScope(user)
客户端 ──GET /api/v1/nodes──► 可见集合 = ⋃ 所属组的节点 ∩ 启用中的节点
```

---

## 1. 改造前的状态与问题

| 维度 | v2.0 现状 |
|---|---|
| 账号 | `role` 只有 `admin` / `user`，无分组概念 |
| 节点 | 单池，`enabled` 是唯一开关 |
| 下发 | `GET /api/v1/nodes` 对所有登录账号返回**同一份**全量启用节点 |
| 落盘 | `users.json` / `tokens.json` / `nodes.json` |

问题：所有账号看到同一份节点，无法做到「A 套餐只给 A 组节点」「内测账号只给测试节点」，
只能靠部署多套服务端绕开。

## 2. 目标与非目标

目标：

1. 管理员可建权限组、配可访问节点、把组分配给账号，全程在网页控制台完成（CLI 也有对应命令）。
2. 节点拉取**按账号所属权限组鉴权**，越权节点绝不下发。
3. 升级不破坏现有功能：老账号、老客户端、老数据目录都要照旧工作。
4. 权限变更能立刻生效，不需要客户端改代码或重新登录。

非目标：

- 不做节点级（单条）授权，只做组级 —— 组已经够用，且管理成本低。
- 不做客户端上传/自助订阅，只读模型保持不变。
- 不引入数据库，继续用文件存储（单机、几百 KB、无外部依赖）。

---

## 3. 权限模型

三层，从上到下依次收窄：

| 层 | 载体 | 作用 |
|---|---|---|
| 角色 | `user.role` = `admin` / `user` | 能否进管理面 |
| 权限组 | `user.groups[]` → `group.nodes[]` | 能拿到哪些节点 |
| 兜底策略 | `acl.json: default_policy` | 没分组的账号怎么办 |

判定表（**唯一权威实现**是 `api.go` 的 `nodeScope`）：

| 账号状态 | 可见节点 | `restricted` |
|---|---|---|
| `role=admin` | 全部启用节点（不受组限制） | `false` |
| 有权限组（1 个或多个） | 各组节点**并集** ∩ 启用节点 | `true` |
| 无权限组 + `default_policy=open` | 全部启用节点（**升级前的行为**） | `false` |
| 无权限组 + `default_policy=closed` | 空 | `true` |

三点设计取舍：

1. **管理员不受限。** 否则管理员把自己分进某个组后，控制台里就只剩那几条节点，
   等于把自己锁在门外。
2. **一个节点可以在多个组里**，一个账号也可以属于多个组，取并集。常见场景：
   「香港」「日本」两个组都放常用节点，套餐组再叠一层。
3. **`open` 是默认值。** 升级前所有账号都没有 `groups` 字段，直接默认 `closed`
   会让线上所有客户端在下次拉取时把节点清空。管理员在控制台点一下「改成 closed」
   即完成收紧，之后新建的账号不给组就真的看不到节点。

### 版本推进（让客户端立刻感知权限变化）

客户端只会比较 `GET /api/v1/nodes?version=N` 的返回值，不会理解 ACL。
所以**任何**权限相关写操作（建/删/改组、改组成员、改账号归属、改策略）都会
`nodes.BumpVersion()`，把节点池版本号推高一格 —— 所有客户端的增量判断必然失效，
下一轮拉取就拿到新结果。代价是一次全量重拉（管理员操作，频率极低）。

---

## 4. 数据模型与迁移

### 4.1 `data/users.json`（增量字段）

```json
{
  "users": {
    "alice": {
      "name": "alice",
      "password": "pbkdf2_sha256$210000$...$...",
      "created_at": 1795162700854,
      "role": "user",
      "groups": ["ea7718b22672"]        // ← 新增，可缺省/为空
    }
  }
}
```

### 4.2 `data/acl.json`（新文件）

```json
{
  "default_policy": "open",
  "updated_at": 1795162723262,
  "groups": [
    {
      "id": "ea7718b22672",
      "name": "套餐A",
      "description": "只给 A 套餐用户",
      "nodes": ["35f242cab85b", "04ad39e8e9ff"],
      "created_at": 1795162713651,
      "updated_at": 1795162713783
    }
  ]
}
```

- 组 `id` 是 12 位十六进制（与节点 id 同一生成器），**改名不改 id**，所以改名不影响授权；
- `nodes` 存节点 id 而不是链接：链接换了节点身份不变，权限跟着节点走；
- 写入走「临时文件 + fsync + rename」，文件 0600，目录 0700，与既有存储一致。

### 4.3 迁移策略：零改写

| 场景 | 行为 |
|---|---|
| 老数据目录（没有 `acl.json`） | 启动时按 `{policy: open, groups: []}` 初始化，**不写盘**，等第一次改动才落盘 |
| 老账号（`users.json` 里没有 `groups`） | 反序列化为 `nil`，等价于未分组；`open` 策略下仍可见全部节点 |
| 新账号 | 不带 `groups` 就是未分组；建号时也可以直接带组 |
| 节点被删 | 自动从所有组里摘掉该 id（否则会留死引用） |
| 权限组被删 | 自动从所有账号的 `groups` 里摘掉该 id |

升级不需要停机，不需要脚本，`data/` 直接沿用；回滚只要换回旧二进制
（旧版本会忽略 `acl.json` 和 `groups` 字段，退回「全量下发」行为）。

---

## 5. 接口

### 5.1 对客户端：形状不变，语义收窄

| 方法 | 路径 | 变化 |
|---|---|---|
| `POST /api/v1/auth/login` | — | `user` 增加 `groups:[{id,name}]`；`nodes.count` 改为**该账号可见的**条数；新增 `nodes.restricted` |
| `GET /api/v1/auth/me` | — | 同上 |
| `GET /api/v1/nodes` | — | `nodes` 只含权限组内的节点；新增 `restricted` 字段 |
| `GET /api/v1/nodes?version=N` | — | 语义不变；但 ACL 变更后 `N` 会失配，客户端会重拉 |
| `PUT /api/v1/vault` | — | 不变，固定 403 |
| `GET /api/v1/health` | — | 不变 |

响应是**纯增量**（只多了 `restricted` 和 `user.groups`），老客户端不认识这两个字段也无影响。
唯一的行为变化就是 `nodes` 数组变短了 —— 正是本次升级的目的。

### 5.2 新增管理端接口（均要求 `role=admin`）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/admin/groups` | `{groups:[{...,node_count}], default_policy, ungrouped:[账号名]}` |
| POST | `/api/v1/admin/groups` | `{name, description?, nodes?:[节点id]}` |
| PATCH | `/api/v1/admin/groups/{id}` | `{name?, description?}` |
| DELETE | `/api/v1/admin/groups/{id}` | 删组，返回 `{detached_users}` |
| POST | `/api/v1/admin/groups/{id}/nodes` | `{ids:[节点id]}` 整体替换该组可访问节点 |
| POST | `/api/v1/admin/users/{name}/groups` | `{groups:[组id]}` 整体替换账号归属，返回 `node_count` |
| POST | `/api/v1/admin/acl/policy` | `{default_policy:"open"\|"closed"}` |

既有接口也补了权限组信息：`GET /admin/users` 每条多了 `groups` / `node_count` / `restricted`，
`POST /admin/users` 支持可选 `groups`，`GET /admin/overview` 多了 `acl` 与 `users.ungrouped`。

### 5.3 命令行

```bash
rekosync -addgroup 套餐A                    # 建组
rekosync -groups                           # 列组（含节点数、账号数）
rekosync -groupnodes "套餐A=id1,id2"        # 配节点（覆盖）
rekosync -usergroups "alice=套餐A,内测"      # 分配（覆盖，留空=清空）
rekosync -acl closed                       # 未分组账号=无节点
rekosync -users                            # 列表里多了「权限组」一列
```

节点 id 从 `-groups` 看不到，用控制台的「配置节点」更方便；CLI 适合脚本化批量操作。

---

## 6. 鉴权实现要点

```go
// api.go —— 全服务唯一的权限判定入口
func (s *Server) nodeScope(u *User) (allow map[string]bool, restricted bool)
```

- **单点判定**：拉取接口、登录响应、`/me`、管理端账号列表里的「可见节点数」全部调它，
  保证界面上显示的数字和客户端真实拿到的一致（不会出现「后台显示 5 个，客户端只有 2 个」）。
- **过滤在下发前**：`NodeStore.ListByIDs(allow)` 先判 `enabled` 再判 `allow`，
  权限组里列了但被停用的节点依然不下发。
- **鉴权在每次请求时算**，不在令牌里缓存，所以改组/改归属对已登录设备立即生效
  （不需要吊销令牌，也不需要用户重新登录）。
- **不信任前端**：控制台只是入口，所有管理接口都有 `requireAdmin` 二次校验。

---

## 7. 错误处理

统一沿用 `{"error":{"code","message"}}` 结构。新增/沿用的码：

| HTTP | code | 触发场景 |
|---|---|---|
| 400 | `bad_request` | 组名为空/超 32 字符/含换行、`default_policy` 非法、`ids` 不是数组 |
| 400 | `unknown_group` | 给账号分配了不存在的组 id（错误消息里列出前 5 个） |
| 400 | `unknown_node` | 给组配置了不存在的节点 id |
| 404 | `group_not_found` | 组 id 不存在 |
| 409 | `duplicate` | 组名重复（忽略大小写，避免「套餐A」与「套餐a」并存） |
| 403 | `forbidden` | 普通账号访问 `/api/v1/admin/*` |
| 401 | `unauthorized` | 令牌无效/已吊销 |

两个「脏数据自愈」的处理（不报错，静默修正）：

- 删节点 → `acl.PruneNodes(ids)` 从所有组摘掉；受影响的组不重置版本则客户端不会重拉，所以一并 bump。
- 删组 → `store.RemoveGroupFromUsers(id)` 从所有账号摘掉。

若账号仍引用了不存在的组（历史脏数据/手工改文件），`Scope` 会把它当「未分组」按兜底策略处理，
行为可预测，不会出现「组没了反而放开了所有节点」的意外。

---

## 8. 兼容性矩阵

| 组合 | 结果 |
|---|---|
| 新服务端 + 老客户端（不带 `?version=`） | 正常，每次全量拉，内容已按权限过滤 |
| 新服务端 + 老客户端（带 `?version=`） | 正常，ACL 变更会 bump 版本触发重拉 |
| 新服务端 + 老 `data/`（无 `acl.json`、账号无 `groups`） | 全部账号维持「可见全部节点」 |
| 旧服务端 + 新 `data/`（有 `acl.json`） | 旧服务端忽略该文件，退回全量下发（回滚路径） |
| 权限组里配了已停用的节点 | 停用节点不下发，恢复启用后自动重新可见（组成员关系还在） |

---

## 9. 测试与验证

### 9.1 自动化单测（`server/acl_test.go`，13 个）

```bash
cd RekoboxForWindows/server
go test ./...              # 全部通过约 3s
go test -run TestACL -v    # 只跑权限相关
```

| 用例 | 验证点 |
|---|---|
| `TestLegacyUsersFileStillSeesAllNodes` | 老 `users.json`（无 `groups`）读入即未分组，`open` 下不受限；且不会凭空创建 `acl.json` |
| `TestACLPersistsAcrossRestart` | 组、成员、策略、账号归属都能落盘并恢复 |
| `TestNodesFilteredByGroup` | 组内账号 2 条、未分组 3 条、管理员 3 条 |
| `TestClosedPolicyHidesEverythingForUngrouped` | `closed` 下未分组 0 条且 `restricted=true` |
| `TestMultiGroupUnionAndDisabledExcluded` | 多组并集；组内停用节点不下发 |
| `TestACLChangeInvalidatesClientVersion` | 改组后旧 `version` 不再 `unchanged` |
| `TestUserGroupChangeInvalidatesClientVersion` | 改归属后同上 |
| `TestAdminEndpointsRejectNormalUser` | 普通账号访问管理接口/给自己改归属 403；无令牌 401 |
| `TestGroupErrors` | 重名 409、空名 400、未知节点 400、未知组 404/400、非法策略 400、方法 405 |
| `TestDeleteGroupDetachesUsers` | 删组摘引用并回落兜底策略 |
| `TestDeleteNodePrunesGroup` | 删节点自动清理组内引用 |
| `TestLoginAndMeReportScopedCount` | 登录/`me` 的 `count` 与 `groups` 正确 |
| `TestRenameGroupKeepsMembership` | 改名不改授权 |

### 9.2 端到端自检（真服务 + curl，25 项断言）

```bash
cd RekoboxForWindows/server
go build -o rekosync .
BIN=./rekosync bash smoke-acl.sh
```

脚本用临时数据目录起服务，覆盖：老账号兼容、组内/组外隔离、多组并集、版本失效重拉、
策略收紧、越权 403、401、409、400、删组摘引用、重启后状态保持。退出码 0 = 全部通过。

### 9.3 手工验证（控制台）

1. `/admin` 登录 → 「权限组」页签 → 建「套餐A」→「配置节点」勾 2 个 → 保存；
2. 「账号管理」→ alice 行「权限组」→ 勾「套餐A」→ 保存，行上「可见节点」应变成 2；
3. 用 alice 登录客户端拉取，只能看到这 2 个节点；换未分组账号仍是全部；
4. 把组里勾选改成 1 个 → 客户端下一轮拉取（或点「立即拉取」）只剩 1 个；
5. 顶部策略切换成 `closed` → 未分组账号拉取为空。

界面已被无头浏览器回归过一遍（登录、四个页签、两个弹窗），无控制台报错。

---

## 10. 上线步骤

```bash
# 1. 备份（data/ 就是全部状态）
tar czf rekosync-$(date +%F).tgz -C /opt/rekosync data

# 2. 更新 —— 两条路选一条
#    a) 源码编译（脚本拉 main 重编译，data/ 不动；起不来自动回滚）
sudo bash <(curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh)
#    b) 预编译二进制（服务器上没有 Go 时更省事）
#       从 releases/tag/server-v2.1.0 取 rekosync_2.1.0_linux_amd64 + SHA256SUMS，校验后：
sudo bash deploy.sh --binary ./rekosync_2.1.0_linux_amd64

# 3. 打开 /admin，确认「权限组」页签出现、账号列表里「可见节点」是真实条数
```

收紧权限的顺序很重要：**先建组并分配账号，最后才把策略切成 `closed`**。
顺序反了会让未分组账号在切策略那一刻掉线（节点被清空），但重新分配后会恢复。

回滚：把旧二进制换回去即可（`rekosync.prev`，部署脚本的更新流程会自动保留）；
`acl.json` 与 `users.json` 里的 `groups` 字段会被旧版本忽略，行为退回全量下发。

---

## 11. 已知限制

1. 组级授权，不能对单条节点做例外（例如「A 组除节点 X 外全部」）。需要时拆组。
2. 组没有嵌套/继承，多个组只是并集，没有「减集」。
3. 管理员始终全量可见，无法用权限组限制管理员自己（管理员的约束手段是别给他发 admin）。
4. 权限变更会让**所有**客户端重拉一次节点（全局版本号机制），账号很多时是一点额外流量；
   单机几百账号的量级可以忽略。
5. `acl.json` 与 `nodes.json` 一样是明文（含节点链接），磁盘权限默认 0600/0700，敏感环境请全盘加密。
