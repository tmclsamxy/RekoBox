# Rekobox for Windows

Rekobox 的 Windows 桌面版：Compose Desktop 界面 + sing-box 内核子进程，界面形制参考
[NekoRay](https://github.com/MatsuriDayo/nekoray)（工具栏 + 分组选项卡 + 节点表 + 底部日志/连接 Dock + 状态栏）。

## 架构

```
┌────────────────────────────┐
│  app (Compose Desktop)     │  界面 / 托盘 / 系统代理 / 提权
└─────────────┬──────────────┘
              │ 写 config.json，起进程
┌─────────────▼──────────────┐        HTTP 127.0.0.1:9090
│  sing-box.exe (子进程)     │◄──────── Clash API：/traffic /logs /proxies /connections
│  tun inbound (Wintun)      │
│  mixed inbound :2080       │
└────────────────────────────┘
```

与 Android 版最大的区别：**核心是独立进程，GUI 只做配置和遥控**。

| 关注点 | Android | Windows |
|---|---|---|
| 内核装载 | gomobile AAR，同进程 | `sing-box.exe` 子进程 |
| 隧道 | `VpnService` fd → hev tun2socks | 核心自建 `tun`（Wintun） |
| 控制面 | AIDL | Clash API |
| 分应用 | VpnService UID 白名单 | 路由规则 `process_name` |
| 权限 | VPN 授权弹窗 | UAC（仅 TUN 需要） |

## 模块

```
core/   纯 JVM：内核进程管理、Clash API 客户端、配置生成、分享链接解析、节点库、订阅
app/    Compose Desktop 界面（NekoRay 形制）+ Windows 平台集成
runtime/windows/   sing-box.exe、wintun.dll（不进 git）
scripts/fetch-core.ps1   自动下载内核与 Wintun
scripts/package.ps1      跑测试 + 打 MSI 到 ../dist
```

## 界面

| 区域 | 内容 |
|---|---|
| 工具栏 | 程序 / 首选项 / 服务器 / 推广 / 文档 / 更新，右侧 Tun 模式、系统代理勾选 |
| 分组选项卡 | `默认` + 每个订阅一个页签，`＋` 新建订阅 |
| 节点表 | 类型 / 地址 / 名称 / 测试结果 / 流量；行尾 `⋯` 菜单：设为当前、启动、测延迟、复制链接、停用、删除 |
| 底部 Dock | `日志` / `连接` 两个页签，可隐藏 |
| 状态栏 | 运行状态 · Socks/HTTP 与 Clash 端口 · 代理/直连计数 · 实时速率 |

菜单对应关系：`程序 → 启动/断开、测试全部延迟、更新全部订阅、跟随系统启动`；
`首选项 → 分组、基本设置、路由（默认/全局）`；`服务器 → 从剪贴板添加、手动输入配置、更新订阅`。

## 快速开始

```powershell
# 1. 拉内核（约 30 MB，含 wintun.dll）
powershell -ExecutionPolicy Bypass -File scripts\fetch-core.ps1

# 2. 编译 + 跑测试（含 `sing-box check` 真校验生成的配置）
.\gradlew.bat build

# 3. 启动界面
.\gradlew.bat :app:run
```

导入节点：工具栏「服务器 → 从剪贴板添加」，或「手动输入配置」粘贴多行链接 / 订阅正文。

命令行验证是否走通：

```bash
curl --proxy http://127.0.0.1:2080 https://ip.sb
```

## 打包

```powershell
.\gradlew.bat :app:createDistributable     # 免安装目录，无需 WiX
powershell -ExecutionPolicy Bypass -File scripts\package.ps1   # 打 MSI，需要 WiX 3.x
```

`runtime/windows` 通过 `nativeDistributions.appResourcesRootDir` 自动带进安装目录，
所以 MSI 装完即可运行，不用再手工拷 `sing-box.exe`。
若用了自定义安装路径且找不到内核，跑 `scripts\install-runtime.ps1 -Dest "<安装目录>"`。

## 支持范围

| 能力 | 状态 |
|---|---|
| 协议解析 | ss / vmess / vless / trojan / hysteria2 / tuic / anytls / socks / http + 原始 outbound JSON |
| 订阅 | HTTP(S) / `file://`，整体 base64 与明文多行，按链接去重 |
| 节点库 | `%LOCALAPPDATA%\Rekobox\state.json`，可手工编辑 |
| 运行模式 | TUN（Wintun，需管理员）/ 系统代理（WinINet）/ 仅本地端口 |
| 系统代理 | 开之前备份原设置，退出还原，不破坏用户原有代理 |
| 控制面 | Clash API：节点切换、测延迟、流量、日志、连接列表 |
| 桌面集成 | 托盘、单实例锁、开机自启、关闭到托盘、退出清理 |
| 单测 | 15 项，含「生成配置交给真内核 `sing-box check`」 |

## 已知限制

- TUN 必须管理员（Wintun 建网卡）；非管理员自动降级到系统代理。
- 系统代理只覆盖 WinINet，走裸 socket 的程序不受影响 —— 这类需求用 TUN。
- 没有 Android 那种「按应用代理」，改用 `route.rules[].process_name` 按进程名分流。
- 路由只做了「默认（绕过局域网）/ 全局」两档；「绕过大陆」需要 geosite 规则集，后续接。
- 流量列暂未按节点统计（sing-box 只给全局与连接级数据）。
- `ConfigBuilder` 的 DNS 结构对齐 sing-box 1.10；升到 1.11+ 需同步改（`servers[].type`）。

## 许可

GPL-3.0，与上游 NekoBox for Android 一致。
