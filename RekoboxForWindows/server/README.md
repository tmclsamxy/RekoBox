# rekosync —— RekoBox 节点池服务端

单文件 Go 服务，**只用标准库**，无数据库、无外部依赖。给客户端提供账号登录与
**只读**节点池，并自带一个网页控制台（`/admin`）供管理员录入和整理节点。

```
管理员 ──浏览器──► /admin  ──► 节点池（data/nodes.json）
                                   │
客户端 ──登录──► GET /api/v1/nodes ─┘   只读，没有上传接口
```

设计细节（接口、权限模型、客户端行为）见仓库根目录
[`docs/04-account-sync.md`](../../docs/04-account-sync.md)。

## 功能

| 能力 | 说明 |
|---|---|
| 账号登录 | 永久令牌；口令 PBKDF2-SHA256（210000 轮）；登录失败按 IP 限速 |
| 节点池 | 管理员按分享链接录入，服务端解析出类型/地址/端口用于展示 |
| 只读下发 | 客户端只有一个数据入口 `GET /api/v1/nodes`；旧的 `PUT /api/v1/vault` 返回 403 |
| 网页控制台 | 节点增删改排序启停、账号增删改禁、概览统计，纯静态单文件内嵌 |
| 数据落盘 | `data/nodes.json` + `data/users.json` + `data/tokens.json`，写入走临时文件+rename |

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
./rekosync -data ./data -users                                    # 列账号
./rekosync -data ./data -passwd  alice                            # 改口令（吊销旧令牌）
./rekosync -data ./data -disable alice                            # 禁用（同时吊销令牌）
./rekosync -data ./data -enable  alice
./rekosync -data ./data -deluser alice
```

管理员也可以在网页控制台里完成上述操作（除创建管理员自身）。

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
| 账号管理 | 新建普通用户/管理员、改口令、禁用/启用、删除 |
| 接入说明 | 客户端怎么填、支持哪些协议、接口一览 |

节点链接支持：`vmess://` `vless://` `trojan://` `ss://` `socks5://` `http(s)://`
`tuic://` `hysteria2://`（含 `hy2://`）`naive+https://`。
录入时会校验协议与地址端口，重复链接直接拒绝；`#名称` 用作节点名。

## 接口

| 方法 | 路径 | 权限 | 说明 |
|---|---|---|---|
| GET | `/api/v1/health` | 无 | 探活 |
| POST | `/api/v1/auth/login` | 无 | `{username,password,device}` → `{token,user.role}` |
| POST | `/api/v1/auth/logout` | 登录 | 吊销当前令牌 |
| GET | `/api/v1/auth/me` | 登录 | 账号信息 + 节点池版本 |
| GET | `/api/v1/nodes` | 登录 | **只读**节点池（`?version=N` 未变时只回 `unchanged`） |
| PUT | `/api/v1/vault` | 登录 | 已废弃，固定 **403**（旧客户端兼容） |
| GET/POST | `/api/v1/admin/nodes` | 管理员 | 全量列表 / 批量录入 |
| PATCH/DELETE | `/api/v1/admin/nodes/{id}` | 管理员 | 改一条 / 删一条 |
| POST | `/api/v1/admin/nodes/batch` | 管理员 | `{ids,action:enable\|disable\|delete}` |
| POST | `/api/v1/admin/nodes/order` | 管理员 | `{ids:[...]}` 重排 |
| GET/POST | `/api/v1/admin/users` | 管理员 | 列表 / 建号 |
| DELETE | `/api/v1/admin/users/{name}` | 管理员 | 删号 |
| POST | `/api/v1/admin/users/{name}/password` | 管理员 | `{password}` |
| POST | `/api/v1/admin/users/{name}/state` | 管理员 | `{disabled:true\|false}` |
| GET | `/api/v1/admin/overview` | 管理员 | 概览统计 |

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
data/users.json      账号（PBKDF2-SHA256 口令哈希；role=user/admin）
data/tokens.json     令牌（只存 SHA-256）
data/nodes.json      节点池 + 全局版本号
data/vaults/*.json   旧版按账号快照，已不再写入（可删）
```

文件权限 0600、目录 0700，全部写入走「临时文件 + fsync + rename」。
备份只需 `tar` 整个 `data/`。
