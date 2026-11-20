# rekosync —— RekoBox 节点池服务端

单文件 Go 服务，**只用标准库**，无数据库、无外部依赖。给客户端提供账号登录与
**只读、按权限组过滤**的节点池，并自带一个网页控制台（`/admin`）供管理员录入节点、
建权限组、分配账号。

```
管理员 ──浏览器──► /admin ──► 节点池（data/nodes.json）
                             权限组（data/acl.json）  group.nodes = [节点id]
                             账号  （data/users.json） user.groups  = [组id]
                                  │
客户端 ──登录──► GET /api/v1/nodes ┘   只读；只返回本账号权限组内的节点
```

设计细节（接口、权限模型、客户端行为）见仓库 `docs/`：

- [`docs/04-account-sync.md`](../../docs/04-account-sync.md) —— 账号与只读节点池
- [`docs/05-acl-permission-groups.md`](../../docs/05-acl-permission-groups.md) —— 权限组升级（模型 / 迁移 / 验证）

## 功能

| 能力 | 说明 |
|---|---|
| 账号登录 | 永久令牌；口令 PBKDF2-SHA256（210000 轮）；登录失败按 IP 限速 |
| 节点池 | 管理员按分享链接录入，服务端解析出类型/地址/端口用于展示 |
| **权限组** | 组 ↔ 节点多对多，账号 ↔ 组多对多；`GET /nodes` 只下发本账号组内的节点，多组取并集；未分组账号按兜底策略（`open` 全量 / `closed` 无）处理 |
| 只读下发 | 客户端只有一个数据入口 `GET /api/v1/nodes`；旧的 `PUT /api/v1/vault` 返回 403 |
| 即时生效 | 权限组或归属一改就把节点池版本 +1，客户端下一轮拉取（含 `?version=` 增量）自动拿到新结果，不用改客户端、不用重新登录 |
| 网页控制台 | 节点增删改排序启停、**权限组增删改与节点勾选**、账号增删改禁与分组、概览统计，纯静态单文件内嵌 |
| 数据落盘 | `data/nodes.json` + `data/acl.json` + `data/users.json` + `data/tokens.json`，写入走临时文件+rename |

## 预编译二进制（Release 附件）

不想在服务器上装 Go 的话，直接从 [server-v2.1.0](https://github.com/tmclsamxy/RekoBox/releases/tag/server-v2.1.0)
下载对应平台的单文件二进制（`CGO_ENABLED=0`、`-trimpath -ldflags "-s -w"`，约 6–7 MB）：

```
rekosync_2.1.0_linux_amd64
rekosync_2.1.0_linux_arm64
rekosync_2.1.0_linux_arm        # 树莓派等 armv7
rekosync_2.1.0_windows_amd64.exe
rekosync_2.1.0_darwin_amd64
rekosync_2.1.0_darwin_arm64
SHA256SUMS                      # sha256sum -c SHA256SUMS 校验
```

```bash
# 校验后直接装（仍然会建 systemd 单元、系统用户、防火墙规则，只是跳过编译）
sha256sum -c SHA256SUMS --ignore-missing
chmod +x rekosync_2.1.0_linux_amd64
sudo bash deploy.sh --binary ./rekosync_2.1.0_linux_amd64
```

## 一键部署（推荐）

### 方式 A：一条命令（服务器上，任意目录）

```bash
curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh | sudo bash
```

如果交互提示没出现（个别终端对管道有限制），换成 stdin 仍是终端的形式：

```bash
sudo bash <(curl -fsSL https://raw.githubusercontent.com/tmclsamxy/RekoBox/main/RekoboxForWindows/server/deploy.sh)
```

脚本启动后是**交互菜单**：

```
  RekoBox 服务端部署工具  v2.1.0
  ──────────────────────────────────────
    系统：Ubuntu 24.04 (x86_64)
    状态：未安装

  1) 安装
  2) 更新到最新版本
  3) 清理卸载（完全删除，可重新安装）
  0) 退出
```

安装时会逐项询问，直接回车用默认值：

| 询问项 | 默认 | 说明 |
|---|---|---|
| 服务端口 | `8080` | 1–65535，会同时写进 systemd、防火墙放行与打印的客户端地址 |
| 只监听 127.0.0.1 | `n` | 前置 nginx/CDN 时选 `y`，不对外暴露端口 |
| 安装目录 | `/opt/rekosync` | 程序与数据都在这里，可自定义 |
| 管理员账号 | `alice` | 登录网页控制台用 |
| 管理员口令 | 自动生成 | 手动输入至少 6 位；留空自动生成 20 位随机口令，只显示一次 |
| 放行防火墙端口 | `y` | 有启用的 ufw/firewalld 时自动放行 |

脚本自己处理这些事：

| 步骤 | 说明 |
|---|---|
| 系统依赖 | 按 apt/dnf/yum/apk 自动装 `curl` `git` `tar` `ca-certificates` |
| Go 工具链 | 没有或低于 1.21 时，从阿里云 / golang.google.cn / go.dev 依次尝试下载安装 |
| 源码 | 脚本旁有 `main.go` 直接用；否则 `git clone`，失败再退回源码包下载 |
| 编译 | `CGO_ENABLED=0 go build -trimpath -ldflags "-s -w"`，`GOPROXY=goproxy.cn` |
| 安装 | 系统用户 `rekosync`、systemd（开机自启 + 崩溃拉起）、写 `/etc/rekobox-install.conf` 记录安装位置 |
| 账号 | 建**管理员**账号（`-addadmin`） |
| 收尾 | 放行防火墙、`/api/v1/health` 自检、打印控制台与客户端地址 |

**更新 / 卸载**：再跑一次同一条命令（或 `bash deploy.sh`），菜单里选 2 / 3。
更新会拉最新源码重编译、保留 `data/`（账号与节点池），新版本起不来会自动回滚到 `rekosync.prev`。
卸载会删除服务、程序、数据、源码缓存、系统用户与防火墙规则，并做残留校验，删干净后可立即重装。

### 常用参数（脚本化 / 远程部署）

```bash
bash deploy.sh -y                                  # 全默认，不交互安装
bash deploy.sh --install --port 9000 --account root # 指定端口与管理员账号
bash deploy.sh --update                            # 更新（自动定位安装目录）
bash deploy.sh --uninstall -y                      # 完全卸载（不确认）
bash deploy.sh --uninstall --keep-data             # 先把数据打包到 /var/backups 再卸载
bash deploy.sh --uninstall                         # 交互确认，需输入 uninstall
bash deploy.sh --listen 127.0.0.1:8080             # 交给 nginx 反代
bash deploy.sh --account bob --reset-password      # 给已有账号重设口令
bash deploy.sh --src /root/Rekobox/RekoboxForWindows/server  # 指定源码目录
bash deploy.sh --binary ./rekosync                 # 直接装现成二进制，跳过编译
bash deploy.sh --skip-deps                         # 不装系统依赖（已备好 go 时）
```

### 方式 B：在开发机远程部署（Windows / macOS）

```bash
cd RekoboxForWindows/server
bash deploy-remote.sh
```

本机有 Go → 交叉编译后只上传二进制（服务器不需要 Go、不需要外网）；
本机没 Go → 上传 `deploy.sh` 交给服务器自行安装与编译。

口令通过 stdin 传给服务器，不出现在命令行与 `ps` 里。

## 手动部署

```bash
# 服务器上（Go 1.21+）
GOPROXY=https://goproxy.cn,direct CGO_ENABLED=0 \
  go build -trimpath -ldflags "-s -w" -o rekosync .

# 或在 Windows 开发机上交叉编译
bash ../libs/build_linux_sync.sh
```

## 账号管理（没有注册接口）

```bash
./rekosync -data ./data -addadmin root  -password 'strong-pass'   # 管理员（可进 /admin）
./rekosync -data ./data -adduser  alice -password 'user-pass'     # 普通用户（只能拉节点）
./rekosync -data ./data -users                                    # 列账号（含权限组）
./rekosync -data ./data -passwd  alice                            # 改口令（吊销旧令牌）
./rekosync -data ./data -disable alice                            # 禁用（同时吊销令牌）
./rekosync -data ./data -enable  alice
./rekosync -data ./data -deluser alice
```

管理员也可以在网页控制台里完成上述操作（除创建管理员自身）。

## 权限组管理

不给账号分配权限组时，行为由 `default_policy` 决定：`open`（默认）可见全部启用节点、
`closed` 什么都看不到。**升级上来的老账号都落在 `open` 这一档**，所以升级本身不改变任何行为。

```bash
./rekosync -data ./data -addgroup 套餐A                    # 建组
./rekosync -data ./data -groups                           # 列组：名称 / ID / 节点数 / 账号数
./rekosync -data ./data -groupnodes "套餐A=id1,id2,id3"     # 覆盖该组的可访问节点
./rekosync -data ./data -usergroups "alice=套餐A,内测"       # 覆盖账号归属（留空 = 清空）
./rekosync -data ./data -acl closed                       # 未分组账号 = 看不到节点
```

节点 id 可以在控制台「节点管理」或 `GET /api/v1/admin/nodes` 里看到。
更省事的做法是直接用网页控制台：权限组页签 → 建组 → 「配置节点」勾选。

收紧权限的正确顺序：**先建组并分配账号，最后再切 `closed`**。

## 运行

```bash
./rekosync -addr :8080 -data ./data
# 启动日志会打印：网页控制台 http://127.0.0.1:8080/admin
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `-addr` | `:8080` | 监听地址；只想让 nginx 反代就写 `127.0.0.1:8080` |
| `-data` | `./data` | 数据目录 |
| `-max-body` | `8388608` | 单请求体上限 |

systemd：见 `rekosync.service`。

## 网页控制台

浏览器打开 `http://<服务器>:8080/admin`，用管理员账号登录。

| 页签 | 能做什么 |
|---|---|
| 节点管理 | 粘贴节点链接批量添加（一行一条）、搜索/筛选、启用停用、上下移排序、编辑名称/分组/备注/链接、复制链接、批量删除 |
| 权限组 | 建组 / 改名 / 删除、给组勾选可访问节点（搜索、全选、清空）、切换未分组账号的兜底策略 |
| 账号管理 | 新建普通用户/管理员、分配权限组、改口令、禁用/启用、删除；显示每个账号的**可见节点数** |
| 接入说明 | 客户端怎么填、权限模型、支持哪些协议、接口一览 |

节点链接支持：`vmess://` `vless://` `trojan://` `ss://` `socks5://` `http(s)://`
`tuic://` `hysteria2://`（含 `hy2://`）`naive+https://`。
录入时会校验协议与地址端口，重复链接直接拒绝；`#名称` 用作节点名。

## 接口

| 方法 | 路径 | 权限 | 说明 |
|---|---|---|---|
| GET | `/api/v1/health` | 无 | 探活 |
| POST | `/api/v1/auth/login` | 无 | `{username,password,device}` → `{token,user.role,user.groups,nodes.count}` |
| POST | `/api/v1/auth/logout` | 登录 | 吊销当前令牌 |
| GET | `/api/v1/auth/me` | 登录 | 账号信息 + 权限组 + **本账号可见的**节点数 |
| GET | `/api/v1/nodes` | 登录 | **只读**、**按权限组过滤**的节点（`?version=N` 未变时只回 `unchanged`） |
| PUT | `/api/v1/vault` | 登录 | 已废弃，固定 **403**（旧客户端兼容） |
| GET/POST | `/api/v1/admin/nodes` | 管理员 | 全量列表 / 批量录入 |
| PATCH/DELETE | `/api/v1/admin/nodes/{id}` | 管理员 | 改一条 / 删一条（删节点会清理组内引用） |
| POST | `/api/v1/admin/nodes/batch` | 管理员 | `{ids,action:enable\|disable\|delete}` |
| POST | `/api/v1/admin/nodes/order` | 管理员 | `{ids:[...]}` 重排 |
| GET/POST | `/api/v1/admin/groups` | 管理员 | 权限组列表（含 `node_count`）/ `{name,description,nodes?}` 建组 |
| PATCH/DELETE | `/api/v1/admin/groups/{id}` | 管理员 | 改名 / 删组（连带摘掉账号归属） |
| POST | `/api/v1/admin/groups/{id}/nodes` | 管理员 | `{ids:[...]}` 设置该组可访问的节点 |
| POST | `/api/v1/admin/acl/policy` | 管理员 | `{default_policy:"open"\|"closed"}` 未分组账号策略 |
| GET/POST | `/api/v1/admin/users` | 管理员 | 列表（含 `groups`/`node_count`）/ 建号（可带 `groups`） |
| DELETE | `/api/v1/admin/users/{name}` | 管理员 | 删号 |
| POST | `/api/v1/admin/users/{name}/password` | 管理员 | `{password}` |
| POST | `/api/v1/admin/users/{name}/state` | 管理员 | `{disabled:true\|false}` |
| POST | `/api/v1/admin/users/{name}/groups` | 管理员 | `{groups:[组id]}` 设置账号归属 |
| GET | `/api/v1/admin/overview` | 管理员 | 概览统计（含 `acl` 与未分组账号数） |

## 快速自检

```bash
curl http://127.0.0.1:8080/api/v1/health

T=$(curl -s -X POST http://127.0.0.1:8080/api/v1/auth/login \
      -H 'Content-Type: application/json' \
      -d '{"username":"root","password":"strong-pass"}' | grep -o '"token":"[^"]*"' | cut -d'"' -f4)

curl -X POST http://127.0.0.1:8080/api/v1/admin/nodes \
     -H "Authorization: Bearer $T" -H 'Content-Type: application/json' \
     -d '{"links":"tuic://uuid:pass@1.2.3.4:25247?congestion_control=bbr#香港","group":"香港"}'

curl -H "Authorization: Bearer $T" http://127.0.0.1:8080/api/v1/nodes
```

## 数据目录

```
data/users.json      账号（PBKDF2-SHA256 口令哈希；role=user/admin；groups=所属权限组）
data/tokens.json     令牌（只存 SHA-256）
data/nodes.json      节点池 + 全局版本号
data/acl.json        权限组 + 未分组账号的兜底策略（升级前不存在，首次改动时自动创建）
data/vaults/*.json   旧版按账号快照，已不再写入（可删）
```

文件权限 0600、目录 0700，全部写入走「临时文件 + fsync + rename」。
备份只需 `tar` 整个 `data/`。

## 测试

```bash
go test ./...                    # 单测（权限组、兼容性、版本失效、错误码）
BIN=./rekosync bash smoke-acl.sh # 端到端自检：起真服务跑 25 项断言
```

两者都不需要外部依赖；`smoke-acl.sh` 用临时目录与临时端口，跑完自动清理。
