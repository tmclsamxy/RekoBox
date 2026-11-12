# rekosync —— RekoBox 节点同步服务端

单文件 Go 服务，**只用标准库**，无数据库、无外部依赖。给 Windows 客户端提供
账号登录与节点快照同步。

设计细节（接口、鉴权、冲突合并、离线策略）见仓库根目录
[`docs/04-account-sync.md`](../../docs/04-account-sync.md)。

## 一键部署（推荐）

在开发机上执行，交互式完成「上传 → 建账号 → 装 systemd → 自检」：

```bash
cd RekoboxForWindows/server
bash deploy.sh
```

会依次询问服务器地址、SSH 用户/端口、安装目录、监听地址、要创建的账号；
回车即用默认值。口令不回显、不落盘、不进 `ps`；留空则自动生成 20 位随机口令并只在最后显示一次。

非交互（CI/批量）：

```bash
HOST=1.2.3.4 USER=root LISTEN=:8080 ACCOUNT=alice PASSWORD='强口令' \
OPEN_FW=y CONFIRM=y bash deploy.sh </dev/null
```

只想拿到 Linux 二进制、不部署：

```bash
bash deploy.sh --local
```

要求：服务器为 root 或配了免密 `sudo -n`；本机有 `ssh`/`scp`（Git Bash 自带）。

## 手动部署

```bash
# 服务器上（Go 1.21+）
GOPROXY=https://goproxy.cn,direct CGO_ENABLED=0 \
  go build -trimpath -ldflags "-s -w" -o rekosync .

# 或在 Windows 开发机上交叉编译
bash ../libs/build_linux_sync.sh
```

## 建账号（没有注册接口）

```bash
./rekosync -data ./data -adduser alice -password 'strong-pass'
./rekosync -data ./data -users
./rekosync -data ./data -passwd  alice
./rekosync -data ./data -deluser alice
```

## 运行

```bash
./rekosync -addr :8080 -data ./data
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `-addr` | `:8080` | 监听地址；只想让 nginx 反代就写 `127.0.0.1:8080` |
| `-data` | `./data` | 数据目录 |
| `-max-body` | `8388608` | 单请求体上限，节点多时按需调大 |

systemd：见 `rekosync.service`。

## 快速自检

```bash
curl http://127.0.0.1:8080/api/v1/health
curl -X POST http://127.0.0.1:8080/api/v1/auth/login \
     -H 'Content-Type: application/json' \
     -d '{"username":"alice","password":"strong-pass"}'
```

## 数据目录

```
data/users.json           账号（PBKDF2-SHA256 口令哈希）
data/tokens.json          令牌（只存 SHA-256）
data/vaults/<user>.json   每个账号一份节点快照 + 版本号
```

文件权限 0600、目录 0700，全部写入走「临时文件 + fsync + rename」。
备份只需 `tar` 整个 `data/`。
