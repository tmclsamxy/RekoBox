# rekosync —— RekoBox 节点同步服务端

单文件 Go 服务，**只用标准库**，无数据库、无外部依赖。给 Windows 客户端提供
账号登录与节点快照同步。

设计细节（接口、鉴权、冲突合并、离线策略）见仓库根目录
[`docs/04-account-sync.md`](../../docs/04-account-sync.md)。

## 一键部署（推荐）

### 方式 A：在服务器上直接跑（全新系统也行）

```bash
# 已有仓库
cd Rekobox/RekoboxForWindows/server && bash deploy.sh

# 没有仓库也行，脚本会自己装依赖、拉源码
bash deploy.sh          # 自动装 curl/git/Go → 编译 → 装 systemd → 建账号 → 自检
```

脚本自己处理这些事：

| 步骤 | 说明 |
|---|---|
| 系统依赖 | 按 apt/dnf/yum/apk 自动装 `curl` `git` `tar` `ca-certificates` |
| Go 工具链 | 没有或低于 1.21 时，从阿里云 / golang.google.cn / go.dev 依次尝试下载安装 |
| 源码 | 脚本旁有 `main.go` 直接用；否则 `git clone`，失败再退回源码包下载 |
| 编译 | `CGO_ENABLED=0 go build -trimpath -ldflags "-s -w"`，`GOPROXY=goproxy.cn` |
| 安装 | 系统用户 `rekosync`、`/opt/rekosync`、systemd（开机自启 + 崩溃拉起） |
| 账号 | 交互输口令（不回显），留空自动生成 20 位并只显示一次 |
| 收尾 | 可选放行 ufw/firewalld，本机 `/api/v1/health` 自检，打印客户端要填的地址 |

常用参数：

```bash
bash deploy.sh -y                          # 全默认，不交互
bash deploy.sh --port 9000                 # 换服务端口（默认 8080）
bash deploy.sh --listen 127.0.0.1:8080     # 交给 nginx 反代
bash deploy.sh --account bob --reset-password   # 给已有账号重设口令
bash deploy.sh --src /root/Rekobox/RekoboxForWindows/server  # 指定源码目录
bash deploy.sh --binary ./rekosync         # 直接装现成二进制，跳过编译
bash deploy.sh --skip-deps                 # 不装系统依赖（已备好 go 时）
```

交互运行时也会单独询问「服务端口」，回车保持 8080；想换端口直接敲数字即可。
端口会同时用于 systemd 的 `-addr`、防火墙放行和最后打印的客户端地址。

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
