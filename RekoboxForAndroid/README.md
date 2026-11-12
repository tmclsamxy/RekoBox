# RekoBox for Android

基于 [NekoBox for Android](https://github.com/MatsuriDayo/NekoBoxForAndroid) 的下游再发行版，
遵循 **GPL-3.0**。核心引擎（sing-box / libcore）与上游一致，本仓库主要改动为应用标识与包名重命名。
上游版权归 MatsuriDayo 及 [AUTHORS](AUTHORS) 中的贡献者所有。

- 包名 `io.rekobox` · minSdk 21 · targetSdk 35
- 构建：AGP 8.8.1 / Gradle 8.10.2 / Kotlin 2.0.21 / NDK r28

## 架构

```
app/                     Android 前端（Kotlin，View + Fragment）
  ├─ bg/                 ProxyService / VpnService / AIDL binder
  ├─ fmt/                各协议分享链接与订阅解析
  ├─ database/           Room 实体与配置存储
  └─ ui/  widget/        界面
libcore/                 Go 模块，封装 sing-box，经 gomobile 编成 libcore.aar
hev-socks5-tunnel/       C 子模块，tun2socks，编成 libhev-socks5-tunnel.so
```

`app/build.gradle.kts` 依赖 `app/libs/libcore.aar` 与 `app/src/main/jniLibs/*/*.so`，
两者都不入库，构建前需先生成。

## 构建

前置：JDK 17、Android SDK（platform 35 / build-tools 35.0.0+）、NDK、Go 1.25+、
[gomobile `master2` 分支](https://github.com/MatsuriDayo/gomobile)（装好后把二进制改名为
`gomobile-matsuri` / `gobind-matsuri`）。`libcore/go.mod` 用 `../../` 引用 sing-box 与 libneko，
两份源码需放在仓库父目录。

```bash
# 1) 原生核心（约 8 分钟，4 个 ABI）
cd libcore
gomobile-matsuri bind -androidapi 21 -cache "$(pwd)/.build" -trimpath -ldflags='-s -w' \
  -tags='with_conntrack,with_gvisor,with_quic,with_wireguard,with_utls,with_clash_api' .
cp libcore.aar ../app/libs/

# 2) tun2socks
cd .. && bash buildScript/compile-hevtun.sh

# 3) 签名与 SDK 路径
cat > local.properties <<'EOF'
sdk.dir=/path/to/Android/Sdk
ndk.dir=/path/to/Android/Sdk/ndk/28.2.13676358
KEYSTORE_PASS=***
ALIAS_NAME=rekobox
ALIAS_PASS=***
EOF

# 4) 打包（oss / fdroid / play / preview）
./gradlew app:assembleOssRelease
```

Windows 本机构建的完整踩坑记录见 `../docs/03-release.md`；一键脚本 `tools/build-release.sh`。

产物：`app/build/outputs/apk/oss/release/RekoBox-<version>-<abi>.apk`（按 ABI 分包）。

## 账号与同步 / Account & Sync

侧边抽屉菜单最上方「账号与同步」，位于「配置」之上。登录后把分组与节点同步到自建服务器，
和 Windows 端共用同一套协议与账号，服务端无需改动。

- 只同步可互转的类型：**socks / http / shadowsocks / vmess / vless / trojan /
  hysteria2 / tuic / naive**；其它类型（hysteria v1、ssr、trojan-go、wireguard、
  chain、config…）在安卓端不可见，但会原样透传回服务端，不会因为安卓同步而丢失
- 令牌永久有效，只有主动注销、服务端改口令或删号才失效
- 断网时改动照常落本地并置 `pending`，恢复后 5s→10s→…→5min 退避自动补推
- 本地状态：`SharedPreferences("rekosync")` + `files/sync/meta.json`
  （uuid 绑定、内容指纹、墓碑、以及无法解析的远端条目）

服务端部署见 `../docs/04-account-sync.md`，实现代码在
`app/src/main/java/io/rekobox/android/sync/`（`SyncModel` 合并 / `SyncMapper` 字段互转 /
`SyncApi` HTTP / `SyncEngine` 调度）。

## 支持的代理协议 / Supported Proxy Protocols

* SOCKS (4/4a/5)
* HTTP(S)
* SSH
* Shadowsocks
* ShadowsocksR
* VMess
* Trojan
* VLESS
* AnyTLS/AnyReality
* Snell 1/2/3/4/5/6
* ShadowTLS
* TUIC
* Juicity
* Hysteria 1/2
* WireGuard
* Trojan-Go (trojan-go-plugin)
* NaïveProxy (naive-plugin)
* Mieru (mieru-plugin)

<details>
<summary>XHTTP Extra TLS配置示例</summary>

<pre><code class="language-json">
{
    "no_grpc_header": false,  // stream-up/one
	"x_padding_bytes": "100-10000",
	"sc_max_each_post_bytes": 1000000, // packet-up only
	"sc_min_posts_interval_ms": 30, // packet-up only
	"xmux": {
		"max_concurrency": "16-32",
		"max_connections": "0-0",
		"c_max_reuse_times": "0-0",
		"h_max_request_times": "600-900",
		"h_max_reusable_secs": "1800-3000",
		"h_keep_alive_period": 0
	},
    "x_padding_obfs_mode": false,
    "x_padding_key": "",
    "x_padding_header": "",
    "x_padding_placement": "",
    "x_padding_method": "",
    "uplink_http_method": "",
    "session_placement": "",
    "session_key": "",
    "session_id_table": "",  // hex/base36/base62/alphabet etc, ""=UUID
    "session_id_length": 0,
    "seq_placement": "",
    "seq_key": "",
    "uplink_data_placement": "",
    "uplink_data_key": "",
    "uplink_chunk_size": 0,
    "congestion_controller": "",  // H3 only: ""=bbr, bbr/cubic/reno
    "cwnd": 0,  // H3 only, default 32
	"download": {
		"mode": "auto",
		"host": "b.yourdomain.com",
		"path": "/xhttp",
        "no_grpc_header": false,  // stream-up/one
	    "x_padding_bytes": "100-10000",
	    "sc_max_each_post_bytes": 1000000, // packet-up only
	    "sc_min_posts_interval_ms": 30, // packet-up only
		"xmux": {
			"max_concurrency": "16-32",
			"max_connections": "0-0",
			"c_max_reuse_times": "0-0",
			"h_max_request_times": "600-900",
			"h_max_reusable_secs": "1800-3000",
			"h_keep_alive_period": 0
		},
        "x_padding_obfs_mode": false,
        "x_padding_key": "",
        "x_padding_header": "",
        "x_padding_placement": "",
        "x_padding_method": "",
        "uplink_http_method": "",
        "session_placement": "",
        "session_key": "",
        "session_id_table": "",  // hex/base36/base62/alphabet etc, ""=UUID
        "session_id_length": 0,
        "seq_placement": "",
        "seq_key": "",
        "uplink_data_placement": "",
        "uplink_data_key": "",
        "uplink_chunk_size": 0,
        "congestion_controller": "",  // H3 only: ""=bbr, bbr/cubic/reno
        "cwnd": 0,  // H3 only, default 32
		"server": "$(ip_or_domain_of_your_cdn)",
		"server_port": 443,
		"tls": {
			"enabled": true,
			"server_name": "b.yourdomain.com",
			"alpn": "h2",
			"utls": {
				"enabled": true,
				"fingerprint": "chrome"
			}
		}
	}
}
</code></pre>
</details>

<details>
<summary>XHTTP Extra Reality配置示例</summary>

<pre><code class="language-json">
{
    "no_grpc_header": false,  // stream-up/one
	"x_padding_bytes": "100-10000",
	"sc_max_each_post_bytes": 1000000, // packet-up only
	"sc_min_posts_interval_ms": 30, // packet-up only
	"xmux": {
		"max_concurrency": "16-32",
		"max_connections": "0-0",
		"c_max_reuse_times": "0-0",
		"h_max_request_times": "600-900",
		"h_max_reusable_secs": "1800-3000",
		"h_keep_alive_period": 0
	},
    "x_padding_obfs_mode": false,
    "x_padding_key": "",
    "x_padding_header": "",
    "x_padding_placement": "",
    "x_padding_method": "",
    "uplink_http_method": "",
    "session_placement": "",
    "session_key": "",
    "session_id_table": "",  // hex/base36/base62/alphabet etc, ""=UUID
    "session_id_length": 0,
    "seq_placement": "",
    "seq_key": "",
    "uplink_data_placement": "",
    "uplink_data_key": "",
    "uplink_chunk_size": 0,
    "congestion_controller": "",  // H3 only: ""=bbr, bbr/cubic/reno
    "cwnd": 0,  // H3 only, default 32
	"download": {
		"mode": "auto",
		"host": "example.com",
		"path": "/xhttp",
        "no_grpc_header": false,  // stream-up/one
	    "x_padding_bytes": "100-10000",
	    "sc_max_each_post_bytes": 1000000, // packet-up only
	    "sc_min_posts_interval_ms": 30, // packet-up only
		"xmux": {
			"max_concurrency": "16-32",
			"max_connections": "0-0",
			"c_max_reuse_times": "0-0",
			"h_max_request_times": "600-900",
			"h_max_reusable_secs": "1800-3000",
			"h_keep_alive_period": 0
		},
        "x_padding_obfs_mode": false,
        "x_padding_key": "",
        "x_padding_header": "",
        "x_padding_placement": "",
        "x_padding_method": "",
        "uplink_http_method": "",
        "session_placement": "",
        "session_key": "",
        "session_id_table": "",  // 可选: hex/base36/base62/alphabet 等, 留空则用 UUID
        "session_id_length": 0,
        "seq_placement": "",
        "seq_key": "",
        "uplink_data_placement": "",
        "uplink_data_key": "",
        "uplink_chunk_size": 0,
        "congestion_controller": "",  // H3 only: ""=bbr, bbr/cubic/reno
        "cwnd": 0,  // H3 only, default 32
		"server": "$(ip_or_domain_of_your_cdn)",
		"server_port": 443,
		"tls": {
			"enabled": true,
			"server_name": "example.com",
			"reality": {
				"enabled": true,
				"public_key": "$(your_publicKey)",
				"short_id": "$(your_shortId)"
			},
			"utls": {
				"enabled": true,
				"fingerprint": "chrome"
			}
		}
	}
}
</code></pre>
</details>

请到[这里](https://matsuridayo.github.io/nb4a-plugin/)下载插件以获得完整的代理支持.

Please visit [here](https://matsuridayo.github.io/nb4a-plugin/) to download plugins for full proxy supports.

## 支持的订阅格式 / Supported Subscription Format

* 一些广泛使用的格式 (如 Shadowsocks, ClashMeta 和 v2rayN)
* sing-box 出站

仅支持解析出站，即节点。分流规则等信息会被忽略。

* Some widely used formats (like Shadowsocks, ClashMeta and v2rayN)
* sing-box outbound

Only resolving outbound, i.e. nodes, is supported. Information such as diversion rules are ignored.

## Credits

Upstream:

- [MatsuriDayo/NekoBoxForAndroid](https://github.com/MatsuriDayo/NekoBoxForAndroid) — 本项目的直接上游

Core:

- [SagerNet/sing-box](https://github.com/SagerNet/sing-box)

Android GUI:

- [shadowsocks/shadowsocks-android](https://github.com/shadowsocks/shadowsocks-android)
- [SagerNet/SagerNet](https://github.com/SagerNet/SagerNet)

Web Dashboard:

- [Yacd-meta](https://github.com/MetaCubeX/Yacd-meta)

## 许可证

[GPL-3.0](LICENSE)。分发二进制时须同时提供完整源码；保留 `LICENSE`、`AUTHORS` 与原版权声明。
