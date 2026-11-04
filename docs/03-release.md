# RekoBox 首发构建与发布记录（v1.0.0）

> 执行时间：2026-11-04 · 机器：Windows 11 · 构建方式：**本机原生**（非 CI）
> 产物：4 个 split APK，15–17 MB，已签名，附 geoip/geosite 数据

---

## 1. 结果

| 项 | 值 |
|---|---|
| 版本 | `1.0.0` / versionCode **5** |
| 包名 | `io.rekobox` |
| 变体 | `oss`（另有 fdroid / play / preview 未构建） |
| APK | arm64-v8a / armeabi-v7a / x86_64 / x86 |
| 大小 | 16 / 17 / 17 / 17 MB |
| 签名 | RSA 4096，SHA-256 `cae8a18c…f96b8e` |
| 构建耗时 | libcore 约 8 min，Gradle ~3 min |
| 产物目录 | `D:/WorkBuddy/Rekobox/dist/` |

Git：提交 `300a004`，tag `v1.0.0`。

---

## 2. 到底需要哪些前置（Windows 本机构建）

Android 版不是纯 Gradle 工程，`app/libs/libcore.aar` 与 `jniLibs/*.so` 都必须先手工产出，
否则 Kotlin 编译阶段直接报 `Unresolved reference: libcore`。

| 依赖 | 版本 | 说明 |
|---|---|---|
| JDK | 17 | AGP 8.8.1 要求；注意本机 `JAVA_HOME` 指向 21，构建时需显式切 17 |
| Android SDK | platform 35 / build-tools **35.0.0** | 仓库声明 35.0.1，本机只装了 35.0.0 → 改 `Helpers.kt` |
| NDK | 28.2.13676358 | 用于 hev tun2socks；CI 用 25.0.8775105 |
| Go | 1.25.4 | `gomobile bind` 编译 sing-box |
| gomobile | `MatsuriDayo/gomobile` @ `master2` | **必须用这个 fork**，官方版 GOBIND 变量对不上 |
| sing-box 源码 | `starifly/sing-box` @ `0b7a2fc` | `libcore/go.mod` 里 `replace => ../../sing-box` |
| libneko 源码 | `starifly/libneko` @ `6a85c18` | 同上 |

> 两个源码仓库要放在**仓库父目录**（`D:/WorkBuddy/Rekobox/`），因为 go.mod 用的是 `../../` 相对路径。

---

## 3. 完整构建步骤

```bash
# 0) 环境（一次性）
#    Go 解压到 D:/rea-tools/go，gomobile 装到 GOPATH/bin 后改名为 *-matsuri
export GOPATH=/d/WorkBuddy/Rekobox/.gopath
export PATH=/d/rea-tools/gopath/bin:/d/rea-tools/go/bin:$PATH
export ANDROID_HOME=/d/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/28.2.13676358
export GOBIND=gobind-matsuri

# 1) libcore.aar（约 8 分钟，4 个 ABI）
cd libcore
gomobile-matsuri bind -androidapi 21 -cache "$(pwd)/.build" -trimpath -ldflags='-s -w' \
  -tags='with_conntrack,with_gvisor,with_quic,with_wireguard,with_utls,with_clash_api' .
cp libcore.aar ../app/libs/

# 2) hev tun2socks（4 个 ABI，约 30 秒）
cd .. && bash buildScript/compile-hevtun.sh

# 3) geoip / geosite（可选，缺了只是 geo 规则不可用）
#    下载 .db 后用 xz 压成 .db.xz 放进 app/src/main/assets/sing-box/

# 4) 签名配置
cat > local.properties <<'EOF'
sdk.dir=D:/Android/Sdk
ndk.dir=D:/Android/Sdk/ndk/28.2.13676358
KEYSTORE_PASS=rekobox2026
ALIAS_NAME=rekobox
ALIAS_PASS=rekobox2026
EOF

# 5) 打包
REKOBOX_NO_RES_SHRINK=1 ./gradlew app:assembleOssRelease
```

一键脚本：`bash tools/build-release.sh oss`。

---

## 4. 踩过的 6 个坑（都已修进仓库）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `gomobile init` 报 `Bad Gateway` / 要求 go1.26 | 它无条件 `go install gobind@latest` | 跳过 init，直接用仓库内编好的 `gobind-matsuri` |
| 2 | `go mod tidy` 报 `Access is denied`（一连串 `-d` / `.lock` 文件） | `%TEMP%`、`%LOCALAPPDATA%` 下的 Go 临时目录被策略拦截 | `TMP`/`TEMP`/`GOCACHE`/`GOPATH` 全部改到工作区内 |
| 3 | `ndk-build: No such file` | Windows 的 NDK 只有 `ndk-build.cmd` | 脚本里加 `.cmd` 回退分支 |
| 4 | `ln: cannot overwrite directory` + 头文件 `unknown type name` | Git Bash 不建真符号链接，`ln -s` 退化成空目录；`core.symlinks=false` 把子模块里的软链签出成**文本文件**（内容就是目标路径） | 复制代替软链 + 扫描「内容像路径的 .h」就地还原 |
| 5 | KSP：`Schema '3.json' not found at app/schemas/io.rekobox…` | 改名漏了 Room schema 目录 | `git mv` 三个 `schemas/<旧包名>.*` 目录到新包名 |
| 6 | `shrinkOssReleaseRes` 报 `resources.txt (拒绝访问)` | AGP 新版资源压缩器在 ABI 分包时多个 worker 并发写同一个 `mapping/<variant>/resources.txt` | 设 `REKOBOX_NO_RES_SHRINK=1` 关资源压缩（R8 裁剪/混淆保留） |

> 坑 6 只在 Windows 本机构建出现，Ubuntu CI 正常，所以正式产物仍建议走 CI。

---

## 5. 签名

```
keystore : release.keystore（仓库根，PKCS12）
alias    : rekobox
storepass: rekobox2026
keypass  : rekobox2026
SHA-256  : cae8a18c2bc7cf86dc32863900a0e28ba3b271bb7e9fc58376fd2bc17af96b8e
```

**这是新生成的密钥，不是上游的**（上游 `release.keystore` 口令未知，无法复用）。

- 必须备份 `release.keystore` 与口令；丢失后已安装用户无法覆盖升级。
- CI 发布需要把上面三个值写进仓库 Secrets：`KEYSTORE_PASS` / `ALIAS_NAME` / `ALIAS_PASS`，
  以及 `LOCAL_PROPERTIES`（local.properties 的 base64）。

---

## 6. 发布到 GitHub

```bash
# 建库（一次性）
curl -X POST -H "Authorization: Bearer $TOKEN" https://api.github.com/user/repos \
  -d '{"name":"RekoBoxForAndroid","private":false}'

# 推送（浅克隆下来的仓库必须先 --unshallow，否则远端 unpack 失败）
git remote add rekobox https://github.com/tmclsamxy/RekoBoxForAndroid.git
git fetch --unshallow origin
git push rekobox main
git push rekobox v1.0.0

# 建 Release：网页上传 dist/*.apk，或用 API
```

CI 路径（推荐后续使用）：`.github/workflows/release.yml` → workflow_dispatch，
输入 tag，`publish` 填 `true` 才真正创建 Release（原逻辑 `!= 'y'` 是反的，已修）。

---

## 7. 遗留

| 项 | 状态 |
|---|---|
| 资源压缩 | Windows 本机构建关闭；CI 构建会开启 |
| geoip/geosite | 已内置（20261012 / 20261103223032），APK +1.4 MB |
| fdroid / play 变体 | 未构建 |
| 与 Android 版 NekoBox 数据互通 | 不支持（包名不同） |
| x86 APK | 仅模拟器用，可不上传 Release |
